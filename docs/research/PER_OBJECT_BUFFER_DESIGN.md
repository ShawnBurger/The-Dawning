# Per-Object Structured Buffer Design (agreed plan, NOT yet implemented)

Survey, design and judging from workflow wf_a9e2859b-170 completed; the
implement stage was killed by a session usage limit (resets 6pm CDT) before
any code was written. This captures the finished design so the work is not
lost. Implement directly from here when budget allows.

**Purpose:** remove the ~341-entity constant-ring ceiling by moving
per-object and per-material data into structured buffers indexed per draw.
This is also the direction SM 6.6 bindless wants, so it must not be built in
a way that has to be undone to get there.

**DECISION: both judge lenses chose Design option 1** (structured buffers
for per-object and per-material data, with the per-draw index supplied as a
root constant; one draw call per entity, only the data location changes).
Design 2 (SV_InstanceID batching by shared mesh) was sound but adds
mesh-grouping complexity for a draw-call reduction that does not help the
ring-pressure problem this task exists to solve. Implement Design 1, applying
the required corrections in the judge sections below.

## Design option 1: Per-draw data into two per-frame StructuredBuffers, indexed by a root constant

**approach**

Keep one draw call per entity and change only where the data lives.

Today three root CBVs are rebound per draw and each costs a 256-byte slice of a FIXED 256 KB ring (`kCBRingSize`), which is what produces the ~341/~170 entity cap. The fix replaces those three per-draw CBV binds with two root SRVs bound ONCE PER PASS plus one 2-DWORD root constant set per draw. The ring stops being the scaling surface entirely: after the change it carries only `CBPerFrame` (256 bytes/frame, flat), and per-draw data lives in growable, `kFrameCount`-instanced upload buffers sized to the actual draw count. The ceiling does not get bigger — it stops existing.

Structural choices, in order of how load-bearing they are:

1. TWO buffers, not one. Object records are 192 bytes and consumed by the VERTEX stage only (`basic_ps.hlsl` never declares b0 — it gets positionWS/normalWS through interpolators). Material records are 80 bytes and consumed by the PIXEL stage only. A root descriptor carries exactly one `ShaderVisibility`, so a single shared buffer would force both to `ALL` and would also have to pick one `StructureByteStride` for two element types 2.4x apart in size. Two buffers keep the existing VERTEX/PIXEL split that root params 0 and 2 already have, cost 2 DWORDs each, and let the two grow independently (object count is 2N because both passes write; material count is N).

2. ROOT SRVs, not descriptor tables. This is decided by `BeginShadowPass` (renderer.cpp:1594-1632): it calls `SetGraphicsRootSignature` and `SetPipelineState` but never `SetDescriptorHeaps`, and the command list is reset fresh each frame (d3d12_device.cpp:452), so NO shader-visible heap is bound during the entire shadow pass. A root SRV is a bare GPU VA and works there with zero additional changes. A table would need a second `SetDescriptorHeaps`+`SetGraphicsRootDescriptorTable` binding point in `BeginShadowPass` that has to stay in sync with `BeginFrame`'s, plus reserved slots out of the 128-descriptor heap — and that heap is the scarce budget (126 usable, ~32 PBR materials, already flagged in ASSET_PIPELINE_SPEC.md:125-128) while the root budget has 56 free DWORDs. Spend DWORDs, not heap slots. It also matches the in-tree DXR precedent exactly: rt_pipeline.cpp:86-113 declares five StructuredBuffer root SRVs and path_tracer.cpp:715-727 binds them, and CLAUDE.md:31-33 explicitly frames those as root SRVs rather than bindless.

3. REPLACE root params 0 and 2 IN PLACE rather than appending and leaving dead params. Param 0 was "per-object data, VERTEX-visible"; it becomes the object StructuredBuffer root SRV — same index, same visibility, same semantic role, only `ParameterType` CBV→SRV and register b0→t0/space2. Param 2 was "material, PIXEL-visible"; it becomes the material root SRV at t0/space3. Params 1, 3, 4 are untouched, so five of the six hardcoded root-parameter indices in the file do not move. The draw index appends as param 5.

4. A NEW `Renderer::BeginFrameResources(D3D12Device&, uint32_t maxDrawsHint)` hoisted ABOVE the shadow pass. The task brief assumes this exists; it does not (repo-wide grep for `BeginFrameResources` returns nothing — the brief describes a tree that is not here). It has to be written. It is not optional polish: the design requires the shadow pass and the main pass to append into ONE per-frame arena with a monotonic cursor, and today the only place the frame slot advances is `BeginFrame` (renderer.cpp:1263-1264), which `App::RenderFrame` calls at app.cpp:1131 — AFTER the shadow pass at app.cpp:1104-1109. Writing this call also fixes a live correctness bug (see frameHazard).

Explicitly NOT doing: no change to `Scene::RenderShadowCasters` / `Scene::RenderEntities` draw logic, no change to `DrawMesh`/`DrawMeshShadow` signatures, no change to the vertex layout or the PSOs, no material deduplication by handle (each draw still gets its own material record — a 1:1 shape preserved deliberately so a later dedup can drop in behind the same index), no `CBPerFrame` change, no touching of `ComputeShadow`'s single-exit shape.

**bufferLayout**

Two element types. `CBPerObject` is renamed `ObjectData` and `CBMaterial` is renamed `MaterialData`; both are confined to renderer.h/renderer.cpp (verified by grep — no other translation unit names them), so the rename is mechanical.

ObjectData — 192 bytes, unchanged field-for-field from today's CBPerObject:

    struct ObjectData
    {
        float worldViewProj[16];      //   0..63   camera VP in the main pass, light VP in the shadow pass
        float world[16];              //  64..127
        float worldInvTranspose[16];  // 128..191
    };
    static_assert(sizeof(ObjectData) == 192, "must match struct ObjectData in basic_vs.hlsl / shadow_vs.hlsl");
    static_assert(offsetof(ObjectData, world) == 64, "...");
    static_assert(offsetof(ObjectData, worldInvTranspose) == 128, "...");

192 = 12 x 16. Every member is 16-byte aligned and nothing straddles a float4 boundary, so cbuffer packing rules and StructuredBuffer packing rules produce identical offsets — the layout does not move at all when it leaves the cbuffer. StructureByteStride 192. Note this struct is currently the ONLY one of the three with no size static_assert; adding one closes that gap, and it matters more now (see risks — a root SRV has no descriptor, so nothing validates the stride at runtime).

MaterialData — 80 bytes, unchanged field-for-field from today's CBMaterial:

    struct MaterialData
    {
        float    albedo[4];             //  0..15
        float    roughness;             // 16
        float    metallic;              // 20
        uint32_t useAlbedoTexture;      // 24
        uint32_t useNormalTexture;      // 28
        uint32_t albedoTextureIndex;    // 32
        uint32_t normalTextureIndex;    // 36
        uint32_t useOrmTexture;         // 40
        uint32_t ormTextureIndex;       // 44
        float    emissive[3];           // 48..59
        float    emissiveStrength;      // 60
        uint32_t useEmissiveTexture;    // 64
        uint32_t emissiveTextureIndex;  // 68
        uint32_t materialPad0;          // 72
        uint32_t materialPad1;          // 76
    };
    static_assert(sizeof(MaterialData) == 80, "...");
    static_assert(offsetof(MaterialData, emissive) == 48, "...");
    static_assert(offsetof(MaterialData, useEmissiveTexture) == 64, "...");

I checked the packing block by block: 0-15 is albedo; 16-31 is roughness/metallic/useAlbedo/useNormal; 32-47 is the four index/flag uints; 48-63 is emissive.xyz + emissiveStrength (float3 starts 16-byte aligned, occupies 12, does not straddle); 64-79 is useEmissive/emissiveIndex/pad0/pad1. Every 16-byte block is exactly filled. So this struct is already straddle-free and its offsets are identical under cbuffer and StructuredBuffer rules — no field needs to move. Keep both pad fields: they are what make the size a multiple of 16, which a StructuredBuffer element wants. StructureByteStride 80.

Storage (following path_tracer.h:110-131 FrameUploadBuffer literally — this is the house pattern, with three existing instances in PathTracer, RTAcceleration and DebugOverlay):

    struct FrameStructuredBuffer
    {
        ComPtr<ID3D12Resource> buffer[kFrameCount];
        uint8_t*               mapped[kFrameCount] = {};
        uint32_t               capacity = 0;   // ELEMENTS, not bytes
        bool Valid() const { return buffer[0] != nullptr; }
        void Reset();   // unmap each mapped slot, release
    };
    FrameStructuredBuffer m_objectBuffer;     // stride 192
    FrameStructuredBuffer m_materialBuffer;   // stride  80
    uint32_t m_objectCursor   = 0;
    uint32_t m_materialCursor = 0;

Creation is a copy of `CreateMappedUploadBuffer` (path_tracer.cpp:21) / `Renderer::CreateConstantBuffers` (renderer.cpp:1073), which are already the same recipe: UPLOAD heap, `D3D12_RESOURCE_DIMENSION_BUFFER`, `D3D12_TEXTURE_LAYOUT_ROW_MAJOR`, created in `D3D12_RESOURCE_STATE_GENERIC_READ`, `SetName` with an indexed name (`L"ObjectDataBuffer[%u]"`, `L"MaterialDataBuffer[%u]"`), persistently mapped with `D3D12_RANGE readRange = {0,0}`, never unmapped outside `Reset()`/`Shutdown()`.

Allocate at Init with a nonzero floor (256 object elements, 128 material elements) so the GPU VA is never 0 even in an empty scene — that removes the need to reason about binding a null root SRV during `DrawSky`, which runs under the main root signature.

Byte accounting per shadowed entity, for footprint (NOT for a cap — see newCeiling): 192 (shadow object record) + 192 (main object record) + 80 (material record) = 464 bytes per frame slot, 1,392 bytes across all three slots. Against today's 768 bytes of a hard-capped 256 KB ring.

Per-frame constant ring after the change: `CBPerFrame` only, 256 bytes, flat, independent of entity count. `AlignCBSize` and `UploadCB` stay exactly as they are and keep their overflow error, which becomes effectively unreachable. `kCBRingSize` is left at 256 KB rather than shrunk — shrinking it is a separate, zero-value cleanup and would only add risk to this change.

**indexingMechanism**

A root 32-bit constants parameter holding TWO uints, set per draw.

    // Root param 5
    D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS
    Constants.ShaderRegister  = 3      // b3
    Constants.RegisterSpace   = 0
    Constants.Num32BitValues  = 2
    ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL

HLSL, declared identically in basic_vs.hlsl, shadow_vs.hlsl and basic_ps.hlsl:

    cbuffer CBDrawIndex : register(b3)
    {
        uint objectIndex;
        uint materialIndex;
    };

Per draw:
- `DrawMesh`:       `cmd->SetGraphicsRoot32BitConstants(5, 2, indices, 0);` where `indices = { m_objectCursor, m_materialCursor }` captured BEFORE the two records are appended.
- `DrawMeshShadow`: same call, `{ m_objectCursor, 0 }`. One API call, both values written, so no stale root state survives into a pass — the same reasoning the existing DrawMeshShadow comment (renderer.cpp:1787-1791) gives for filling `world`/`worldInvTranspose` it does not read.

Two values in one parameter rather than two parameters: one `SetGraphicsRoot32BitConstants` call per draw instead of two, same 2-DWORD cost, and one register to keep track of. They stay SEPARATE fields rather than one shared index because within the main pass they differ by the shadow pass's record count, and because keeping the material index independent is what lets a later "index materials by ecs::Material handle instead of by draw" change drop in without touching the object side.

Net per-draw command count goes DOWN: two `SetGraphicsRootConstantBufferView` calls removed from `DrawMesh`, one removed from `DrawMeshShadow`, one `SetGraphicsRoot32BitConstants` added to each. `DrawMesh` goes 2 -> 1, `DrawMeshShadow` stays 1.

WHY NOT SV_InstanceID — this is the single most likely way this change fails silently, so it is worth stating in the design rather than discovering it. On D3D12 at SM 5.1, `SV_InstanceID` does NOT include `StartInstanceLocation`; it always begins at 0 per draw. `StartInstanceLocation` only becomes readable as `SV_StartInstanceLocation` at SM 6.8. So the obvious trick of passing a draw index through the 5th argument of the existing `DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0)` calls (renderer.cpp:1415, renderer.cpp:1802) compiles clean, runs, raises no debug-layer message, and hands every draw index 0 — the entire scene renders with the first entity's transform. The root constant has none of that ambiguity.

WHY NOT a per-instance vertex stream — it means editing `kVertexLayout`, which is shared by the main PSO (renderer.cpp:967) and the shadow PSO (renderer.cpp:1554), and adding a second vertex buffer bind per draw. More surface, more state, no benefit over 1 root DWORD.

The root constant is also the SM 6.6 answer, not a stopgap: bindless changes how textures are addressed, not how a draw identifies its own record.

**rootSigChange**

Both branches of `Renderer::CreateRootSignature` must be edited in lockstep — the v1.1 branch at renderer.cpp:787-852 and the v1.0 fallback at renderer.cpp:853-905 are two independent copies of the same layout, and the fallback only runs when `CheckFeatureSupport` reports no 1.1 support (renderer.cpp:728-735), so a divergence would not reproduce locally.

CHANGED IN PLACE (index and visibility preserved, so no call site renumbers):

- Param 0: `D3D12_ROOT_PARAMETER_TYPE_CBV` b0/space0 -> `D3D12_ROOT_PARAMETER_TYPE_SRV`, `Descriptor.ShaderRegister = 0`, `Descriptor.RegisterSpace = 2`, `Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE` (v1.1 only), `ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX` (unchanged). 2 DWORDs before, 2 after.
- Param 2: `TYPE_CBV` b2/space0 -> `TYPE_SRV`, `ShaderRegister = 0`, `RegisterSpace = 3`, `Flags = DATA_VOLATILE`, `ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL` (unchanged). 2 DWORDs before, 2 after.

UNCHANGED: param 1 (CBV b1, PIXEL, 2), param 3 (SRV table t0-t127 space0, PIXEL, 1), param 4 (SRV table t0 space1 shadow map, PIXEL, 1).

ADDED:

- Param 5: `D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS`, `Constants.ShaderRegister = 3`, `Constants.RegisterSpace = 0`, `Constants.Num32BitValues = 2`, `ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL`. 2 DWORDs.

Array grows `rootParams[5]` -> `rootParams[6]` in both branches.

DWORD TOTAL: 8 -> 10 of 64. 54 free. Breakdown: 2 (object SRV) + 2 (per-frame CBV) + 2 (material SRV) + 1 (texture table) + 1 (shadow table) + 2 (draw-index constants). Static samplers remain free.

`D3D12_ROOT_SIGNATURE_FLAGS` unchanged — `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | DENY_HULL | DENY_DOMAIN | DENY_GEOMETRY` all remain valid; nothing needed to widen to a denied stage.

`DATA_VOLATILE` and not `DATA_STATIC_WHILE_SET_AT_EXECUTE` on both new root SRVs, deliberately: `BeginFrame` binds the material buffer's VA before `DrawMesh` has written any material records into it, and the object buffer's VA is bound at pass start while records are appended throughout the pass. `DATA_STATIC_WHILE_SET_AT_EXECUTE` promises the contents do not change after the root argument is set, which this design violates by construction. The v1.0 fallback has no per-descriptor flags and v1.0 semantics are volatile by default, so the two branches stay behaviourally equivalent rather than merely similar.

REGISTER ALLOCATION. space0 t0-t127 is the material texture table, space1 t0 is the shadow map. The comment at basic_ps.hlsl:62-63 states the shadow map got its own space specifically so it could not collide with the material table "however large that grows"; the new buffers follow that precedent — object buffer at t0/space2, material buffer at t0/space3. b0 and b2 become permanently free (nothing declares them after this change); the draw index takes b3 rather than reusing b0, so a diff cannot confuse "b0 is per-object data" with "b0 is a draw index".

TWO STALE STRINGS TO FIX WHILE IN THIS FUNCTION, since both will mislead the next person budgeting root space: the header comment at renderer.cpp:717-723 claims "Total: 7 DWORDs" and lists only 4 slots (it predates the shadow table), and the success log at renderer.cpp:924 claims 9. The true figure today is 8 and after this change 10.

ALSO IN THIS FUNCTION, benign but adjacent: renderer.cpp:773 builds `const D3D12_STATIC_SAMPLER_DESC staticSamplers[] = { staticSampler, shadowSampler };` BEFORE assigning `staticSampler.RegisterSpace` and `staticSampler.ShaderVisibility` on lines 774-775, so those two assignments mutate a copy nothing reads and s0 actually ships as `SHADER_VISIBILITY_ALL` rather than the intended PIXEL. Harmless (ALL is a superset, RegisterSpace 0 was already the zero-init value) but the lines read as if they took effect. Moving them above line 773 is a one-line reorder; do it or delete them, but do not leave them.

**frameHazard**

Both buffers are `kFrameCount`-instanced and indexed by `device.FrameIndex()`, cached ONCE per frame into `m_currentFrame`. This is not a preference — fencing is not an available alternative here. The only per-frame wait is `WaitForCurrentFrame` (d3d12_device.cpp:431), which waits on `m_fenceValues[m_frameIndex]` only, i.e. the frame `kFrameCount-1` back that last used this back-buffer index. It does NOT wait for frames N+1 or N+2. Instancing is exactly what makes that wait sufficient. And no resource barrier can substitute: these are CPU writes to persistently mapped UPLOAD memory, and barriers order GPU work, not CPU writes — stated verbatim at path_tracer.h:98-108 and debug_overlay.h:70-73.

THE NEW ENTRY POINT:

    void Renderer::BeginFrameResources(D3D12Device& device, uint32_t maxDrawsHint);

Called UNCONDITIONALLY from `App::RenderFrame`, immediately after `m_renderer.ReclaimTextureDescriptors(m_device)` at app.cpp:1066 — above the `if (renderedPathTracing)` split, so both branches advance it. It does exactly four things, in order:

1. `m_currentFrame = device.FrameIndex();`
2. `m_cbOffset = 0;` `m_objectCursor = 0;` `m_materialCursor = 0;`
3. `EnsureFrameStructuredBuffer(device, m_objectBuffer, 2 * maxDrawsHint, sizeof(ObjectData), L"ObjectDataBuffer")` and the same for `m_materialBuffer` at `maxDrawsHint` and `sizeof(MaterialData)`. The 2x on objects is because both passes append.
4. Latch high-water counters for the smoke markers.

`maxDrawsHint` comes from a new `Scene::MeshInstanceCount()` (declared in scene.h, defined in scene.cpp per RULE 2) returning the `MeshInstance` pool's `Count()`. That is an upper bound including invisible entities, which is exactly what you want for sizing.

Steps 1 and 2 MOVE OUT of `BeginFrame` (renderer.cpp:1263-1264). `BeginFrame` keeps everything else, including the `UploadCB(&perFrame, ...)` for b1, which now lands in the correct slot.

THIS FIXES A LIVE BUG, not just an inefficiency. Today `BeginFrame` is the only place `m_currentFrame` and `m_cbOffset` are assigned, and `App::RenderFrame` runs the entire shadow pass (app.cpp:1104-1109) before calling it (app.cpp:1131). So every `DrawMeshShadow` -> `UploadCB` in frame N writes into `m_cbUploadBuffers[frame index of N-1]` at offsets continuing past N-1's high-water mark, and its root CBVs point into a buffer belonging to a different frame slot. `BeginFrame` then rewinds a DIFFERENT buffer to 0. Two consequences: on frame 0 `m_currentFrame` and `m_cbOffset` are both 0 before and after `BeginFrame`, so the main pass overwrites the shadow constants at the same addresses before the GPU executes anything — frame 0's shadow map is rasterised from main-pass bytes. In steady state, frame N+3 reuses that slot and rewinds to 0, while `WaitForCurrentFrame` at the top of frame N+3 only guarantees frame N has retired, not N+1 or N+2. It survives today only because a constant entity count makes the two watermarks abut; the smoke growth test adding 80 entities is exactly the perturbation that breaks it. Hoisting the advance above the shadow pass is the fix, and the new design depends on it — the shadow pass and the main pass must append into ONE arena, this frame's, from cursor 0.

It also fixes the path-tracing leak: app.cpp:1069 never calls `BeginFrame`, so `m_cbOffset`/`m_currentFrame` go stale across RT frames and the first shadow pass after an F1 toggle back to raster writes at whatever offset the last raster frame left. Calling `BeginFrameResources` unconditionally closes that.

GROWTH SAFETY. `EnsureFrameStructuredBuffer` is a direct copy of `PathTracer::EnsureFrameUploadBuffer` (path_tracer.cpp:460-498), same shape line for line: early-out when `target.Valid() && elementCount <= target.capacity`; `newCapacity = elementCount + 64` headroom so growth amortises; allocate ALL `kFrameCount` replacements FIRST and `replacement.Reset(); return false;` on any failure so the live buffers are never left half-swapped; only then, per slot, Unmap old / null the mapped pointer / `device.DeferredRelease(target.buffer[i])` / move the replacement in. Growth therefore never destroys a buffer a recorded command list may still reference — `DeferredRelease` pushes at `m_globalFenceValue + 1` (d3d12_device.cpp:568) and drains from `MoveToNextFrame`.

Critically, growth happens ONLY in `BeginFrameResources`, before any pass records anything and before either root SRV is bound. It never happens mid-pass, so an already-bound root SRV VA can never be invalidated under a recorded draw.

OVERFLOW. If a draw somehow exceeds capacity anyway (the hint under-counted), `DrawMesh`/`DrawMeshShadow` log an error once and RETURN WITHOUT RECORDING THE DRAW. That is strictly better than today's behaviour: `UploadCB` returns GPU address 0 on overflow (renderer.cpp:1237-1241) and all three callers bind that zero straight into `SetGraphicsRootConstantBufferView` with no guard — renderer.cpp:1385, :1410 and especially :1799 where the upload is nested inline in the bind — so the draw is still recorded and reads address zero. A skipped draw is visible and clean; the smoke harness already throws on any `[ERR ]` line (smoke_test.ps1:93-96).

STALE-STATE GUARD. Add a `bool m_frameResourcesBegun` set in `BeginFrameResources` and cleared at the end of the frame, asserted in `UploadCB`, `DrawMesh` and `DrawMeshShadow`. Without it, a future pass added above `BeginFrameResources` reintroduces exactly the bug being fixed here, silently.

SHUTDOWN. `App::Shutdown` already does `WaitForGpu()` (app.cpp:1222) before `Renderer::Shutdown`, so unmap-and-release in `Reset()` is safe. Add both buffers to `Renderer::Shutdown` (renderer.cpp:38-48) alongside the existing CB unmap loop. Device loss must drain rather than wait — that path already lives in `ProcessDeferredReleases`, and since these buffers retire through `DeferredRelease` they inherit it with no new escape hatch needed.

**shaderChanges**

All three raster shaders keep compiling under FXC vs_5_1 / ps_5_1 with `D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS` (shader_utils.cpp:28-37). `StructuredBuffer` and SRVs in the vertex stage are both available at SM 5.0+; nothing here needs 6.x.

basic_vs.hlsl — replace the b0 cbuffer:

    struct ObjectData
    {
        column_major float4x4 worldViewProj;
        column_major float4x4 world;
        column_major float4x4 worldInvTranspose;
    };
    StructuredBuffer<ObjectData> objectBuffer : register(t0, space2);

    cbuffer CBDrawIndex : register(b3)
    {
        uint objectIndex;
        uint materialIndex;
    };

and in `main`:

    ObjectData obj = objectBuffer[objectIndex];
    output.positionCS = mul(obj.worldViewProj, float4(input.position, 1.0));
    output.positionWS = mul(obj.world, float4(input.position, 1.0)).xyz;
    output.normalWS   = normalize(mul((float3x3)obj.worldInvTranspose, input.normal));

`VSInput` and `VSOutput` are untouched — no `SV_InstanceID`, no per-instance stream, no new interpolator. The pixel shader does not need the object record, so `VSOutput` gains nothing.

shadow_vs.hlsl — byte-identical `ObjectData` declaration and the same `objectBuffer`/`CBDrawIndex` declarations, then:

    return mul(objectBuffer[objectIndex].worldViewProj, float4(input.position, 1.0));

The file header comment currently explains that reusing b0 keeps the two passes from drifting in how they interpret per-object constants. That reasoning survives intact and should be reworded rather than deleted — the two passes now share one struct, one buffer and one root parameter, which is a stronger version of the same guarantee.

basic_ps.hlsl — replace the b2 cbuffer:

    struct MaterialData
    {
        float4 albedo;
        float  roughness;
        float  metallic;
        uint   useAlbedoTexture;
        uint   useNormalTexture;
        uint   albedoTextureIndex;
        uint   normalTextureIndex;
        uint   useOrmTexture;
        uint   ormTextureIndex;
        float3 emissive;
        float  emissiveStrength;
        uint   useEmissiveTexture;
        uint   emissiveTextureIndex;
        uint2  materialPad;
    };
    StructuredBuffer<MaterialData> materialBuffer : register(t0, space3);
    cbuffer CBDrawIndex : register(b3) { uint objectIndex; uint materialIndex; };

then `MaterialData mat = materialBuffer[materialIndex];` at the top of `main`, and every bare reference becomes a member access: `albedo`->`mat.albedo`, `roughness`->`mat.roughness`, `metallic`->`mat.metallic`, `useNormalTexture`->`mat.useNormalTexture`, `normalTextureIndex`->`mat.normalTextureIndex`, `useAlbedoTexture`, `albedoTextureIndex`, `useOrmTexture`, `ormTextureIndex`, `emissive`, `emissiveStrength`, `useEmissiveTexture`, `emissiveTextureIndex`. Roughly 14 substitutions, all inside `main`.

`ComputeShadow` is NOT touched. Its single-exit shape is load-bearing — FXC reports X4000 "potentially uninitialized" for returns inside branches and /WX makes that fatal (the comment at basic_ps.hlsl:95-98 says so). It reads only `lightViewProj` from b1 and the shadow map, none of which move.

`ApplyNormalMap` gains a `uint textureIndex` argument already — its signature is unchanged; the caller passes `mat.normalTextureIndex`.

sky_ps.hlsl, tonemap_ps.hlsl, the bloom shaders: untouched. sky_ps runs under the main root signature (`DrawSky` only swaps the PSO) and declares only b1, so the new params being bound is invisible to it.

THE ONE THING TO GET RIGHT — MATRIX PACKING. The engine's transforms are correct only because of a double transpose that cancels, and core/types.h:15-43 documents this at length: `Mat4x4` is ROW-MAJOR storage with row-vector semantics, `UploadCB` memcpys those raw row-major bytes, HLSL cbuffers default to COLUMN-MAJOR packing so the GPU reinterprets the upload as the transpose, and `mul(matrix, vector)` in the shader is the column-vector form. The two transposes cancel exactly. types.h explicitly warns that `#pragma pack_matrix(row_major)` or `/Zpr` on either side, or "fixing" the shader to `mul(v, M)`, breaks every transform in the engine with no compile error and no validation message.

FXC keeps the same column-major default for matrices inside StructuredBuffer elements, so `mul(M, v)` stays correct and the memcpy stays byte-identical. But this is precisely the kind of thing that silently transposes, and relying on a default here would be relying on the exact behaviour types.h warns about. So the design annotates every matrix member `column_major` EXPLICITLY. That is semantically a no-op against today's behaviour — which is the point: it locks the contract in the source instead of in a compiler default, it is legal on struct members under FXC 5.1, and it does not change the 64-byte-per-matrix stride either way. Verification is in the test plan, against the DXR path as the oracle the spec names.

**shadowPassHandling**

The shadow pass's second 256-byte copy of `CBPerObject` does NOT go away as a concept — it goes away as a ring allocation. The shadow pass keeps writing a full per-object record; it just appends a 192-byte element into the shared, growable object buffer instead of consuming 256 bytes of the fixed 256 KB ring.

Concretely:

`BeginShadowPass` (renderer.cpp:1594) gains exactly one line, after the existing `SetGraphicsRootSignature` at renderer.cpp:1629:

    cmd->SetGraphicsRootShaderResourceView(0, m_objectBuffer.buffer[m_currentFrame]->GetGPUVirtualAddress());

No `SetDescriptorHeaps`, no descriptor table, no heap slot reserved. This is the whole argument for root SRVs over tables: `BeginShadowPass` never binds a shader-visible heap and the command list is reset fresh each frame, so during the entire shadow pass no CBV_SRV_UAV heap is bound at all. That is invisible today only because `shadow_vs.hlsl` uses nothing but b0. Route per-object data through a descriptor table and the shadow pass breaks with a debug-layer error about binding a table with no heap set; route it through a root SRV and nothing else in that function changes.

Root params 1, 2, 3 and 4 stay unbound during the shadow pass, exactly as today. Still legal: the shadow PSO has no pixel shader and all four are PIXEL-visible. Param 2 changing from a CBV to an SRV does not affect that.

`DrawMeshShadow` (renderer.cpp:1780-1803) keeps its signature and its structure. It still builds the SAME record the main pass uses, with `worldViewProj = worldMatrix * m_lightViewProj`, and it still deliberately fills `world` and `worldInvTranspose` even though `shadow_vs.hlsl` reads neither. The existing comment's reasoning gets stronger, not weaker: the struct is shared, and a persistently mapped buffer that is reused across frames without clearing is exactly where stale bytes turn a later shader change into a heisenbug. The body becomes:

    const uint32_t index = m_objectCursor;
    if (index >= m_objectBuffer.capacity) { log once; return; }   // skip the draw, do not bind garbage
    ObjectData* dst = reinterpret_cast<ObjectData*>(m_objectBuffer.mapped[m_currentFrame]) + index;
    // ... fill worldViewProj / world / worldInvTranspose exactly as today ...
    ++m_objectCursor;
    const uint32_t indices[2] = { index, 0u };
    cmd->SetGraphicsRoot32BitConstants(5, 2, indices, 0);
    cmd->IASetVertexBuffers(...); cmd->IASetIndexBuffer(...); cmd->DrawIndexedInstanced(...);

Note the write is straight into mapped memory — no staging `ObjectData` on the stack and no memcpy of the whole struct, though a local + one memcpy is equally fine and arguably clearer.

CURSOR SHARING BETWEEN THE PASSES. `m_objectCursor` is reset once per frame in `BeginFrameResources` and runs monotonically across BOTH passes. The shadow pass runs first, so it occupies elements [0, N); the main pass occupies [N, 2N). The two ranges are DISJOINT — each pass owns its own records, and no record is shared between passes.

That is a deliberate choice and worth being explicit about, because the alternative was tempting. `Scene::RenderShadowCasters` (scene.cpp:146) and `Scene::RenderEntities` (scene.cpp:173) walk the same `MeshInstance` pool in the same order with identical visibility/Transform/Material filters, so draw i is the same entity in both passes. One could exploit that to store ONE record per entity carrying both clip matrices (camera and light), cutting object storage in half. Two reasons not to:

1. It is not currently possible without reordering the frame. `m_lightViewProj` is refreshed in `BeginShadowPass` via `UpdateLightMatrix`, but `m_viewProj` is not computed until `BeginFrame` — which runs AFTER the shadow pass. At shadow time the camera view-projection does not exist yet, so `DrawMeshShadow` cannot write the main pass's matrix.
2. It would make that incidental cross-pass ordering property load-bearing for CORRECTNESS. Today it is enforced nowhere — it is a coincidence of two loops happening to filter identically. If shadow casting ever gains a filter the main pass lacks (frustum culling, a `castsShadow` flag, an LOD cut), shared indices silently desynchronise and every object renders with a different entity's transform. Disjoint per-pass ranges make the design depend on the two loops only for BUFFER SIZING, where being wrong means "grew a bit too much" rather than "wrong transforms". That is the right place to be depending on a coincidence.

The cost of not sharing is 192 extra bytes per entity per frame slot. Against a cap that no longer exists, that is the correct trade.

One filter oddity to leave alone: `RenderShadowCasters` requires a `Material` component (scene.cpp:160) even though the shadow pass never reads material data. It means an entity without a Material casts no shadow. Harmless, and it happens to be what keeps the two loops' counts equal — which the new `shadow_records == main_records` smoke assertion will now actively police rather than assume.

**newCeiling**

The entity ceiling imposed by the constant ring is REMOVED, not raised. That is the honest headline; a factor-improvement number would misdescribe the change.

The old arithmetic, which the spec (ASSET_PIPELINE_SPEC.md:108-117) and the measurements agree on: 256 bytes for `CBPerFrame` plus 768 bytes per shadowed entity (256 `CBPerObject` + 256 `CBMaterial` + 256 for the shadow pass's copy, each rounded up by `AlignCBSize`) against a hard `kCBRingSize` of 256 KB. `(262144 - 256) / 768 = 340.9`, i.e. 341 entities, and a four-cascade shadow pass roughly halves it to ~170. Cross-checked against the reported measurements: 97 entities x 768 = 74,496 bytes = 28.4% of 262,144, which is the spec's "29%". 17 x 768 = 13,056 = 5.0%, which is the spec's "5%". The model is right.

After the change, the ring carries `CBPerFrame` alone: 256 bytes per frame, FLAT, independent of entity count. Ring occupancy becomes 0.1% and stops being a function of scene size at all. Per-draw data moves to buffers whose size is `EnsureFrameStructuredBuffer`'d to the actual draw count with 64 elements of headroom, so the binding constraint becomes upload memory rather than a compile-time constant.

New cost model, per shadowed entity, per frame slot:
  192 (shadow object record) + 192 (main object record) + 80 (material record) = 464 bytes
Across `kFrameCount` = 3 slots: 1,392 bytes per entity of persistently mapped UPLOAD memory.

Worked figures:
  -    341 entities (the OLD ceiling):    158 KB per slot,   475 KB total. Fits with room to spare in what used to be one frame's ring.
  -  1,000 entities:                      453 KB per slot,   1.33 MB total.
  -  5,000 entities (a station interior): 2.21 MB per slot,  6.64 MB total.
  - 20,000 entities:                      8.85 MB per slot,  26.6 MB total.

A station interior at a few thousand draws costs single-digit megabytes of upload memory. That is not a constraint worth designing around, which is the point.

If one insists on an equivalent-budget number for comparison: at the same 256 KB per frame slot, 262,144 / 464 = 564 entities, up from 341 — but the buffer is no longer capped at 256 KB, so 564 is not a ceiling, it is just where the old budget would have landed.

Note also that a four-cascade shadow pass, which used to roughly halve the ceiling, now simply adds 192 bytes per entity per additional cascade. Four cascades cost 192 x 4 + 192 + 80 = 1,040 bytes per entity per slot; at 5,000 entities that is 5.0 MB per slot, 14.9 MB total. The cascade upgrade stops being a scaling decision.

WHAT THE REAL CEILING BECOMES. Two things, both untouched by this work and both worth naming so the next measurement targets the right thing: (1) CPU draw-call submission cost, since this design deliberately keeps one `DrawIndexedInstanced` per entity — instancing and merging are the next lever, and this change is a prerequisite for both rather than an obstacle; (2) the 128-descriptor raster texture heap, 126 usable, roughly 32 PBR materials, which ASSET_PIPELINE_SPEC.md:125-128 already flags as something real content exhausts immediately. This design spends root DWORDs (8 -> 10 of 64) precisely so it spends ZERO heap slots and does not make (2) worse.

**bindlessAlignment**

Strictly toward SM 6.6 bindless. There is nothing here that has to be undone to get there, and two things that are prerequisites.

What bindless actually changes is TEXTURE addressing: `Texture2D<float4> materialTextures[MAX_RASTER_TEXTURES] : register(t0)` plus a bound descriptor table becomes `ResourceDescriptorHeap[mat.albedoTextureIndex]` with no table and no root parameter. It does not change where per-draw data lives. So the two buffers this design introduces are the same buffers a bindless renderer would have.

Three specific ways this moves in the right direction:

1. The material record already stores ABSOLUTE shader-visible heap indices. `DrawMesh` writes `albedoTextureIndex = albedoTexture->descriptor.index` straight from the generational `DescriptorHandle` (renderer.cpp:1397-1407), and the material table is bound at heap slot 0 (renderer.cpp:1318) so shader index == absolute heap index. That is already the bindless form. Under SM 6.6 the exact same `uint` feeds `ResourceDescriptorHeap[...]` instead of `materialTextures[...]` — the record does not change, only the one line in the pixel shader that consumes it. Moving that record from a per-draw cbuffer into a StructuredBuffer is the step that makes it a real GPU-side material table rather than a transient upload.

2. A per-draw index in a root constant is the pattern bindless keeps, not one it replaces. Every bindless renderer needs the draw to identify its own record; the root constant is how that is done at every shader model. The alternative that bindless-era hardware offers — `SV_StartInstanceLocation` — needs SM 6.8, which is well past 6.6. So the root constant is not a 5.1 workaround with an expiry date.

3. It converges the two render paths instead of adding a third style. The DXR side already does exactly this: `rt_pipeline.cpp:86-113` declares five `StructuredBuffer` root SRVs and `path_tracer.cpp:715-727` binds them with `SetComputeRootShaderResourceView`, with per-instance material lookup via a global material StructuredBuffer indexed by `InstanceID()`. CLAUDE.md:31-33 states plainly that these are root SRVs and not SM 6.6 bindless, and that no `ResourceDescriptorHeap` appears anywhere in the project. The raster path copying that pattern means both paths reach the bindless migration in the same shape, and the migration becomes one change to texture addressing on both sides rather than two unrelated rewrites.

Root SRVs are fully legal under SM 6.6 and remain the recommended way to bind large per-draw arrays — bindless does not deprecate them. If a future pass wants the buffers themselves heap-indexed rather than root-bound, that is a two-line change (drop params 0 and 2, add their SRVs to the heap, index them from a root constant) and it is optional, not forced.

The one thing this design deliberately does NOT pre-build is instancing. Records stay 1:1 with draws rather than being keyed by material handle or merged across identical meshes. That is a real future win (the same `ecs::Material` currently re-uploads a record for every entity using it) but it requires `ecs::Material` handle plumbing through `scene.cpp`, and folding it in here would mean two unrelated failure modes in one change. The index indirection is exactly what makes it a drop-in later: the material index stops being "draw ordinal" and becomes "material handle" with no change to the buffer, the root signature, or the shaders.

Test plan:
- Compile-time layout enforcement, no new test file needed. Put `static_assert(sizeof(ObjectData) == 192)`, `static_assert(sizeof(MaterialData) == 80)` and `offsetof` asserts for every field of both structs directly in renderer.h next to the existing `CBPerFrame` assert. Both structs are standard-layout so `offsetof` is well-defined in C++20. This is the ONLY guard that exists: a root SRV has no descriptor, so unlike a table-bound SRV there is no `StructureByteStride` for the runtime to validate against the shader's computed stride. `CBPerObject` has no size assert today; this closes that.
- Build clean: cmake -S . -B build then cmake --build build --config Debug, filtered for ' error ' and 'warning C[0-9]'. Zero of both. Shader compilation is part of runtime init, not the build, so a clean build does NOT prove the HLSL compiled - watch the log for the FXC error path, since /WX makes every warning fatal and the three edited shaders are the ones at risk.
- Unit tests unchanged and passing: ./build/Debug/TheDawningTests.exe, checking 'cases  ' and 'RESULT'. This change touches no math or ECS invariant, so any movement here means something unintended was hit.
- Both smoke modes must pass, and the RASTER one is the one with teeth for this change: powershell -NoProfile -ExecutionPolicy Bypass -File tools/smoke_test.ps1 -RasterOnly, then the full run without -RasterOnly. The full run is not merely 'also RT' - it renders RASTER frames with 97 entities (17 demo + the 80 `RTGrowth_*` entities created at frame 8 in ApplySmokeRTMutationStress) before the RT delay elapses and F1 flips at app.cpp:682-690. That 97-entity raster window is exactly the 29%-of-ring measurement in the spec, so it is the at-scale regression test. Confirm dxcompiler.dll and dxil.dll are in build/Debug first or the DXR run fails for an unrelated reason.
- THE PRIMARY CORRECTNESS TEST - PIXEL IDENTITY. This is a pure data-relocation change: the same matrices, the same material values, the same shading math, the same draw order. The rendered image must not move. Capture the back buffer via the existing smoke capture path before and after the change with an identical scene and camera, and diff. Byte-identical (or trivially near, if any float path reorders) is the acceptance criterion. This single test catches the matrix-transpose failure, the wrong-index failure, and the material-field-offset failure at once, which no code inspection reliably does.
- MATRIX PACKING, verified against the oracle the spec names rather than by reasoning. core/types.h:15-43 documents that transforms are correct only because a row-major memcpy and HLSL's column-major cbuffer default transpose each other and cancel. Toggle F1 between raster and path tracing on the demo scene and confirm the geometry does not shift, and specifically check a TRANSLATED, ROTATED, NON-UNIFORMLY SCALED object - types.h:42-43 says explicitly that a cube at the origin will not surface a packing error. The demo spins continuously, which helps. A transpose bug here produces plausible-looking but wrong geometry, not a crash.
- Add the instrumentation the task brief assumed already exists. There is no `[SMOKE] cb_ring_peak` marker anywhere in this tree and no 75% gate in tools/smoke_test.ps1 - the only ring instrumentation is the overflow error at renderer.cpp:1237-1239, which fires at 100% after draws are already reading address zero. Track peaks in the Renderer and emit alongside the other per-run markers near app.cpp:736: `[SMOKE] cb_ring_peak=<bytes> cb_ring_pct=<n> object_records_peak=<n> material_records_peak=<n> shadow_records=<n> main_records=<n> object_capacity=<n>`. The `[SMOKE] key=value` parser at smoke_test.ps1:111 picks these up with no parser change.
- Assert on those markers in smoke_test.ps1, mirroring the existing Assert-Marker style. (a) Fail if `cb_ring_pct` >= 75 - the early-warning gate the spec describes and this tree lacks; after the change it should read ~0 since the ring holds only the 256-byte CBPerFrame, so this doubles as proof the per-draw traffic actually left the ring. (b) Assert `shadow_records == main_records` and both > 0 - this is the cross-pass parity invariant from scene.cpp:146 and :173, which is currently an unenforced coincidence of two loops filtering identically; asserting the invariant is better than asserting a magic entity count that breaks when the demo scene changes. (c) In the full (non -RasterOnly) run assert `main_records >= 97`, confirming the growth window was actually rendered through the raster path.
- Verify growth and reuse explicitly, since `EnsureFrameStructuredBuffer` is the piece most likely to be subtly wrong. The full smoke run already exercises it: 17 entities at start, 97 at frame 8, back to 17 at frame 16 (m_smokeGrowthEntities destroyed). Confirm the buffer grows once, that `object_capacity` stays at its grown value afterwards (it must not shrink - shrinking would mean destroying a buffer a live command list references), and that `PendingDeferredReleaseCount` returns to zero by shutdown. The existing `descriptors_pending_after_renderer_shutdown=0` assertion is the model.
- Confirm the frame-ordering fix rather than assuming it. Add a debug-only assert on `m_frameResourcesBegun` in UploadCB / DrawMesh / DrawMeshShadow and verify it never fires, including across an F1 toggle in both directions - the path-tracing branch at app.cpp:1069 never called BeginFrame, so before this change m_cbOffset and m_currentFrame went stale across RT frames and the first shadow pass after toggling back wrote at a leftover offset. Toggle F1 raster->RT->raster in the full smoke run and confirm clean.
- Confirm the v1.0 root-signature fallback at renderer.cpp:853-905 was edited in lockstep with the v1.1 branch at :787-852. It CANNOT be exercised locally - it only runs when CheckFeatureSupport reports no 1.1 support - so this is a read-both-diffs-side-by-side check, not a runtime one. Adding a parameter to only one branch produces a signature mismatch on older drivers that no local run would catch. Also fix the two stale DWORD strings while in the function (renderer.cpp:717-723 says 7, renderer.cpp:924 says 9, truth is 8 today and 10 after).
- Update ASSET_PIPELINE_SPEC.md's 'Known scaling limits' section. Its constant-ring paragraph (lines 108-123) becomes wrong the moment this lands, and it is the document the next agent reads first. Replace the ~341/~170 figures with the new cost model and state plainly that the ring cap is gone and the binding constraints are now CPU draw submission and the 128-slot texture heap.

Negative test: Two mutations, targeting the two ways this change can pass every build and still be broken.

MUTATION 1 - THE INDEX IS NEVER UPDATED. This is the failure mode with the highest prior, because it is what happens if `SetGraphicsRoot32BitConstants` is called on the wrong root slot, called with the wrong offset argument, or (the classic) if someone later "simplifies" it to `SV_InstanceID` and it silently reads 0 for every single-instance draw at SM 5.1. Every draw then reads element 0 and the whole scene renders with the first entity's transform. Nothing crashes, no validation fires, and the frame still looks like a rendered scene.

Simulate it by deleting the `SetGraphicsRoot32BitConstants` call from `DrawMeshShadow` (or hardcoding `objectIndex = 0` in shadow_vs.hlsl) and running `tools/smoke_test.ps1 -RasterOnly`. The expected result is that `[SMOKE] shadow_written_fraction` collapses: all casters pile onto one transform, so the 256x256 centred probe at the middle of the shadow map (renderer.cpp:1716-1724) loses most of its written texels. That marker already exists and is already read by the harness (app.cpp:1194-1196, smoke_test.ps1:150), so this needs no new instrumentation - it just needs the baseline value recorded before the change and a threshold assertion added after. If the fraction does NOT move materially, the test has no teeth and the threshold is wrong; tighten it until the mutation is caught, then restore. This is the same methodology the existing `shadow_map_written` marker was built with, which the comment at smoke_test.ps1:144-149 says was verified by deleting the caster draw and watching it flip.

Do the equivalent for the main pass by hardcoding `objectIndex = 0` in basic_vs.hlsl and confirming the back-buffer capture diverges from the baseline. Since the primary acceptance criterion is pixel identity against the pre-change capture, the mutation must break that identity - if it does not, the capture comparison is not actually comparing anything.

MUTATION 2 - THE MATRICES SILENTLY TRANSPOSE. Change `column_major` to `row_major` on the three `float4x4` members of `ObjectData` in basic_vs.hlsl. Every transform in the engine should visibly break - objects displaced, rotated wrongly, scale skewed - and the capture diff should be enormous. If the image does NOT change, the annotation is not doing what the design assumes and the packing contract is being enforced by something else (or by nothing), which must be understood before shipping. types.h:36-41 lists this exact substitution as one of the two ways to break every transform with no compile error, so it is a known-good mutation with a known-good expected outcome.

Both mutations must be reverted before the build is called green. Neither should be left in as a permanent test - they are one-shot checks that the assertions have teeth.

Risks: MATRIX PACKING SILENTLY TRANSPOSING. The engine's transforms are correct only because a row-major CPU memcpy and HLSL's column-major cbuffer default cancel each other (core/types.h:15-43). FXC keeps the same column-major default for matrices inside StructuredBuffer elements, so mul(M,v) stays correct - but that is a default, and types.h explicitly warns that changing either side of the cancellation breaks every transform with no compile error and no validation message. Mitigated by annotating every matrix member `column_major` EXPLICITLY rather than relying on the default, and verified by pixel-identity capture plus the F1/DXR visual oracle. This is the highest-consequence risk in the change. | NOTHING VALIDATES THE STRUCT STRIDE AT RUNTIME. A root SRV is a bare GPU VA with no descriptor, so unlike a table-bound SRV there is no `Buffer.StructureByteStride` for the runtime or debug layer to check against the shader's computed struct size. If the C++ struct and the HLSL struct ever disagree by a byte, every element after the first reads shifted garbage and nothing reports it. Mitigated by static_asserts on size AND offsetof for every field of both structs in renderer.h - which also fixes the fact that CBPerObject is currently the only one of the three with no size assert at all. The DXR path has the same exposure today (rt_pipeline.cpp:86-113), so this is house-consistent, not a new class of risk - but it is worth naming because the mitigation is the only guard. | THE v1.0 ROOT-SIGNATURE FALLBACK DIVERGING. renderer.cpp:787-852 and :853-905 are two independent copies of the same layout with different struct types, and the fallback only runs when CheckFeatureSupport reports no 1.1 support. Editing one and not the other produces a signature mismatch on older drivers that no local run - including both smoke modes - would ever catch. Mitigation is a side-by-side diff review, not a test. | THE TASK BRIEF DESCRIBES A TREE THAT DOES NOT EXIST. `Renderer::BeginFrameResources` is not in this repository (repo-wide grep for 'FrameResources', case-insensitive, returns nothing), there is no `[SMOKE] cb_ring_peak` marker, and tools/smoke_test.ps1 contains no ring assertion and no 75% threshold. The measured 5%/29% figures are documented in the spec but not instrumented in code. All of that has to be WRITTEN, not preserved. Anyone estimating this as 'move two structs into buffers' will under-scope it by the frame-advance entry point plus the instrumentation plus the harness assertions. | A LIVE PRE-EXISTING BUG IS BEING FIXED AS A SIDE EFFECT, which means this change cannot be evaluated purely as a refactor. Today the shadow pass uploads into the PREVIOUS frame's ring buffer past that frame's high-water mark, because BeginFrame is the only place the slot advances and it runs after the shadow pass. Frame 0's shadow map is rasterised from main-pass bytes, and in steady state frame N+3 rewinds a slot whose shadow constants frames N+1/N+2 may still be reading, since WaitForCurrentFrame only covers the frame kFrameCount-1 back. It survives today because a constant entity count makes the watermarks abut. Consequence for review: 'the image is identical before and after' is the acceptance criterion for the raster look, but frame 0's shadow map legitimately SHOULD change, and a reviewer expecting bit-identity on the very first frame will read the fix as a regression. | FORGETTING TO CALL BeginFrameResources REINTRODUCES THE BUG SILENTLY. Any future pass added above it in App::RenderFrame, or a new render branch that skips it the way the path-tracing branch at app.cpp:1069 currently skips BeginFrame, puts the frame slot back out of sync with no symptom until entity counts grow. Mitigated by calling it unconditionally at the top of RenderFrame and by a `m_frameResourcesBegun` guard asserted in UploadCB/DrawMesh/DrawMeshShadow - without the guard this is a landmine for whoever adds the next pass. | BUFFER SIZING DEPENDS ON A COINCIDENCE, THOUGH ONLY WEAKLY. RenderShadowCasters and RenderEntities (scene.cpp:146, :173) walk the same pool with identical filters, which is why 2 x maxDrawsHint is enough object capacity. That parity is enforced nowhere. Deliberately mitigated by giving each pass its OWN disjoint index range rather than sharing records, so a future divergence (frustum culling, a castsShadow flag, LOD) costs a buffer growth rather than wrong transforms - and by asserting `shadow_records == main_records` in the smoke harness so the divergence surfaces as a test failure rather than a rendering artifact. | MEMORY GROWTH IS NOW UNBOUNDED WHERE IT USED TO BE CAPPED. Removing kCBRingSize as the limiter means a pathological scene allocates upload memory proportional to draw count with no ceiling at all - 20,000 entities is ~27 MB across three slots, fine, but there is no longer anything that fails fast. The +64-element headroom amortises growth but never shrinks, so a transient spike permanently raises the footprint for the process lifetime. Acceptable and consistent with the three existing FrameUploadBuffer instances, which behave identically, but it is a real change in failure character: from 'hard cap at 341' to 'degrades with memory'. | BASIC_PS.HLSL TAKES ~14 MECHANICAL SUBSTITUTIONS (albedo -> mat.albedo and so on) inside main(). Each one is individually trivial and collectively easy to get 13-out-of-14 right, and a missed one will not compile (the bare identifier no longer exists) - so this is low risk for silent failure but high risk for iteration churn against /WX. Do it in one pass, not incrementally. | TWO ADJACENT DEFECTS SIT IN THE CODE BEING EDITED AND WILL BE TEMPTING TO FIX. renderer.cpp:773-775 builds the static sampler array before assigning staticSampler.RegisterSpace/.ShaderVisibility, so s0 ships as SHADER_VISIBILITY_ALL rather than the intended PIXEL (benign, but the lines read as if they took effect); and basic_ps.hlsl:22 claims CBPerFrame is static_assert'd at 112 bytes when it is 176 (112 is the prefix sky_ps.hlsl declares, which is the actual append-only constraint). Both are one-line fixes in files already being touched - fix them, but keep them out of the same commit as the buffer change so a bisect can separate them.

## Design option 2: Instanced per-draw structured buffers (mesh+texture-set batching, SV_InstanceID + per-batch root constant)

**approach**

Replace the three per-draw root CBVs with two growable, kFrameCount-instanced StructuredBuffers bound as root SRVs, indexed per draw, and issue ONE instanced draw per group of entities sharing a mesh and a texture set.

Five moving parts:

1. ONE object record shared by both passes. Today CBPerObject stores a pre-multiplied worldViewProj, which is why the shadow pass must upload a second copy with the light matrix substituted. Store `world` and `normalMatrix` only, and move the view-projection to a tiny per-PASS cbuffer (b3) whose contents are m_lightViewProj in BeginShadowPass and m_viewProj in BeginFrame. The shadow pass's second 256-byte copy per entity does not shrink — it ceases to exist. Both passes read the same bytes, which is a strictly stronger form of the invariant shadow_vs.hlsl's header comment already asserts.

2. Matrices as explicit float4 rows, never float4x4. render/mesh.h:69-82 documents this exact mistake being made once in this tree (RTInstanceData::normalMatrix), and path_trace.hlsl:635-637 reconstructs with dots. Follow it. The CPU stores the transpose (scene.cpp:525-528 already has the idiom: `out[row*4+c] = m[c][row]`), the shader does `float3(dot(worldT0,p), dot(worldT1,p), dot(worldT2,p))`.

3. Per-draw index = per-batch root 32-bit constant + SV_InstanceID. SV_InstanceID at SM 5.1 on D3D12 does NOT include StartInstanceLocation. This design never uses StartInstanceLocation — it stays 0. SV_InstanceID is a genuine within-draw counter over a real multi-instance draw, and globalIndex = drawBase + SV_InstanceID. When a batch has one instance this degenerates to a plain per-draw root constant, i.e. exactly the non-instanced alternative, so batching is upside and never a load-bearing assumption.

4. Batch key is (Mesh*, albedoIdx, normalIdx, ormIdx, emissiveIdx). Mesh because VB/IB must match. Texture indices because SM 5.1 has no NonUniformResourceIndex: `materialTextures[mat.albedoTextureIndex]` must be wave-uniform, which is free today (root CBV, constant per draw) and is exactly what batching would break. Crucially the SCALAR material fields — albedo colour, roughness, metallic, emissive colour/strength — stay fully per-instance, because they are plain data with no descriptor indexing. So RedSphere and GoldSphere batch together despite differing in colour, roughness and metallic. That is where most per-entity variation actually lives, so the key is far less restrictive than "same material".

5. One pool walk, not two. Scene::RenderShadowCasters and Scene::RenderEntities currently walk the same pool with identical filters in the same order, and the whole design depends on that coincidence. Collapse them into Scene::SubmitDraws, which gathers a flat DrawItem list; Renderer::BuildDrawBatches sorts it, writes both buffers, and produces the batch list; RenderShadowBatches and RenderBatches consume it. The coincidence becomes a single list. It also fixes scene.cpp:160, where an entity without a Material silently casts no shadow.

Prerequisite that does not exist and must be written: Renderer::BeginFrameResources. Grep confirms no such symbol. Today BeginFrame is the only place m_currentFrame/m_cbOffset are set, and app.cpp runs the whole shadow pass before it — so shadow constants land in the PREVIOUS frame's buffer past its high-water mark, governed by the wrong fence. This is a live correctness bug, and the new design makes it unignorable (the shadow pass now needs the frame slot for its structured-buffer reads). BeginFrameResources caches the slot, rewinds the ring, and clears the draw list; it is called unconditionally at app.cpp:1066, above the raster/RT branch, so an F1 toggle also lands on a clean slot.

**bufferLayout**

Two new structs, in a new header so the pure-CPU batcher unit test can include them without pulling in D3D12.

struct GPUObjectRecord           // src/render/gpu_draw_records.h
{
    // Camera-relative object-to-world, TRANSPOSED into three float4 rows, so the
    // VS produces component i with dot(row[i], float4(pos,1)). The fourth row of
    // the transpose is always (0,0,0,1): every matrix reaching the renderer is an
    // affine TRS from Transform::ToCameraRelativeMatrix, the same assumption
    // D3D12_RAYTRACING_INSTANCE_DESC::Transform already makes engine-wide.
    //
    // Explicit rows, NOT float4x4. StructuredBuffer elements do not get the
    // column-major reinterpretation cbuffer matrices do - render/mesh.h:76-78 says
    // so, and it says so because this tree already shipped that bug once.
    float world[12];          //  0..47
    float normalMatrix[12];   // 48..95   InverseTranspose3x3, same transposed shape
};
static_assert(sizeof(GPUObjectRecord) == 96, "must match ObjectRecord in shaders/object_common.hlsli");

struct GPUMaterialRecord         // field-for-field the existing CBMaterial
{
    float    albedo[4];             //  0..15
    float    roughness;             // 16
    float    metallic;              // 20
    uint32_t useAlbedoTexture;      // 24
    uint32_t useNormalTexture;      // 28
    uint32_t albedoTextureIndex;    // 32
    uint32_t normalTextureIndex;    // 36
    uint32_t useOrmTexture;         // 40
    uint32_t ormTextureIndex;       // 44
    float    emissive[3];           // 48..59
    float    emissiveStrength;      // 60
    uint32_t useEmissiveTexture;    // 64
    uint32_t emissiveTextureIndex;  // 68
    uint32_t pad0;                  // 72
    uint32_t pad1;                  // 76
};
static_assert(sizeof(GPUMaterialRecord) == 80, "must match MaterialRecord in shaders/basic_ps.hlsl");

De-risking argument for the material record: CBMaterial's byte layout is IDENTICAL under cbuffer packing rules and StructuredBuffer tight-packing rules, because no member straddles a 16-byte boundary under tight packing (float3 emissive sits at offset 48, which is already 16-aligned). So this is a pure re-binding of an already-verified layout - the bytes do not move, only the register does. Keep pad0/pad1: they make the stride 80, a multiple of 16.

struct CBPerPass                 // new, stays a cbuffer, stays in the CB ring
{
    float viewProj[16];   // camera viewProj (main pass) OR lightViewProj (shadow pass)
};
static_assert(sizeof(CBPerPass) == 64, "must match cbuffer CBPerPass (b3)");

CBPerPass stays a cbuffer precisely so it keeps today's proven reinterpretation: memcpy row-major Mat4x4, declare float4x4, default column-major packing, mul(M, v). Zero new risk there. All the new risk is concentrated in the two structured buffers, where explicit float4 rows remove it.

Packing/stride sizes: object stride 96, material stride 80. Per entity per frame: 176 bytes, versus 768 today.

CBPerFrame is UNTOUCHED - 176 bytes, static_assert intact, sky_ps.hlsl's deliberate 112-byte prefix unaffected, append-only constraint preserved. b3 is a new, separate cbuffer, not an extension of b1, specifically so sky_ps.hlsl needs no edit.

SHARPEST EDGE IN THE DESIGN: a root SRV takes a bare GPU virtual address - SetGraphicsRootShaderResourceView has no StructureByteStride parameter and there is no SRV descriptor to cross-check. The shader's declared struct is therefore the ONLY definition of the stride, and nothing in the toolchain validates it against sizeof() on the C++ side. A mismatch reads garbage silently. Mitigation is discipline: shaders/object_common.hlsli is the single HLSL definition (included by both VS files so they cannot drift), gpu_draw_records.h is the single C++ one, each carries a comment naming the other, and both static_asserts quote the counterpart. This is worth stating loudly in review because it is invisible to the debug layer.

**indexingMechanism**

globalIndex = drawBase + SV_InstanceID.

drawBase is a D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS at b0, Num32BitValues = 1, VERTEX visibility, set once per BATCH with SetGraphicsRoot32BitConstant(0, batch.firstInstance, 0). One DWORD in the root, zero bytes of the ring, zero descriptor-heap slots.

SV_InstanceID is a real within-draw instance counter, because these are real multi-instance draws: DrawIndexedInstanced(mesh.indexCount, batch.instanceCount, 0, 0, 0). StartInstanceLocation stays 0 in every call, in both passes, permanently.

THE TRAP, stated so nobody re-introduces it: on D3D12 at SM 5.1, SV_InstanceID does NOT include StartInstanceLocation - that only becomes reachable as SV_StartInstanceLocation in SM 6.8. Passing a draw index as the 5th argument of DrawIndexedInstanced compiles, runs, raises no debug-layer error, and hands every draw index 0. Every entity then reads structured-buffer element 0 and the whole scene renders with the first entity's transform and material. This design cannot fall into that because it never uses StartInstanceLocation for anything; the index arrives through the root, and SV_InstanceID only ever counts within a genuinely instanced draw.

The VS computes the index once and passes it to the PS as `nointerpolation uint drawIndex : TEXCOORD3` on VSOutput/PSInput, so the pixel shader reaches the material record without needing its own copy of the root constant and without the root constant needing PIXEL visibility.

Contiguity is the constraint this buys with: a batch's records must be contiguous in the buffer, because one base plus a 0..N-1 counter addresses a contiguous run. BuildDrawBatches therefore writes records in sorted order, not pool order, and a batch is closed whenever the key changes. Today both passes draw the identical set so one buffer and one batch list serve both. The designed-in escape hatch if a future frustum-culled main pass diverges from the shadow set: a second StructuredBuffer<uint> indirection, index = g_DrawIndices[drawBase + SV_InstanceID], 4 bytes per visible draw per pass. It costs nothing to leave out now and it is exactly the shape GPU culling and ExecuteIndirect want later.

**rootSigChange**

Seven parameters, 11 DWORDs of 64 (up from 5 / 8). Slots 1, 3 and 4 keep their numbers so BeginFrame's three existing binds are untouched; slots 0 and 2 change TYPE, which is safe because every call site that referenced them as CBVs is being deleted in this change anyway.

idx | before                        | after                                              | DWORDs | visibility
----|-------------------------------|----------------------------------------------------|--------|-----------
 0  | CBV b0 (CBPerObject)          | 32BIT_CONSTANTS b0, Num32BitValues=1 (drawBase)    | 2 -> 1 | VERTEX
 1  | CBV b1 (CBPerFrame)           | unchanged                                          | 2      | PIXEL
 2  | CBV b2 (CBMaterial)           | SRV root descriptor, t0 space3 (material records)  | 2      | PIXEL
 3  | SRV table t0-t127 space0      | unchanged                                          | 1      | PIXEL
 4  | SRV table t0 space1 (shadow)  | unchanged                                          | 1      | PIXEL
 5  | -                             | SRV root descriptor, t0 space2 (object records)    | 2      | VERTEX
 6  | -                             | CBV b3 (CBPerPass, viewProj)                       | 2      | VERTEX

Total 1+2+2+1+1+2+2 = 11. 53 DWORDs free.

Both branches must move in lockstep - v1.1 at renderer.cpp:806 and the v1.0 fallback at renderer.cpp:870 are two independent copies and a driver without 1.1 support would otherwise get a silently divergent signature that no local run catches. In v1.1 both new root SRVs take D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE (contents are rewritten every frame) and so does the new b3 CBV; in v1.0 there is no Flags member and volatile is the implicit default, so the fallback stays behaviourally equivalent. D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS has no flags in either version.

Register spaces: object records at t0 space2, material records at t0 space3. Separate spaces following the precedent basic_ps.hlsl:62-63 states explicitly for the shadow map - so neither can ever collide with the 128-entry material table however large it grows. space0 t0-t127 and space1 t0 are already taken.

Root SRVs, not descriptor tables, and the decisive reason is BeginShadowPass: it calls SetGraphicsRootSignature and SetPipelineState but never SetDescriptorHeaps, the command list is reset fresh each frame, and the shadow pass runs before BeginFrame - so NO shader-visible heap is bound for the entire shadow pass. A root SRV works there with zero additional changes. A table would need SetDescriptorHeaps plus a SetGraphicsRootDescriptorTable added to BeginShadowPass, a second binding point to keep in sync with BeginFrame's, reserved fixed heap slots (bumping m_textureAllocator.Init's firstIndex and shifting every absolute material index), and - because these buffers GROW - either kFrameCount descriptors or the full Release/Reclaim fence discipline, since volatile descriptors are read at GPU execution time. Root SRVs sidestep all of it, and they spend from the abundant budget (53 of 64 DWORDs free) instead of the scarce one (126 usable heap slots, which ASSET_PIPELINE_SPEC.md:125-128 already calls ~32 PBR materials that real content exhausts immediately).

While in CreateRootSignature, fix three pre-existing defects: the header comment at 717-723 claims 7 DWORDs and lists only 4 slots (predates the shadow table), the success log at 924 claims 9, and the truth today is 8 (11 after). Also renderer.cpp:773-775 builds the static sampler array BEFORE assigning staticSampler.RegisterSpace and .ShaderVisibility, so those two lines are dead and s0 ships as SHADER_VISIBILITY_ALL rather than PIXEL - harmless, but it reads as if it took effect.

**frameHazard**

These are CPU writes into persistently mapped UPLOAD memory. No resource barrier can protect them - barriers order GPU work, they do not synchronise CPU writes. path_tracer.h:98-108 and debug_overlay.h:70-73 both state this verbatim; it is settled house doctrine here.

So both buffers are kFrameCount-instanced and indexed by device.FrameIndex(), following PathTracer::FrameUploadBuffer literally: buffer[kFrameCount] + mapped[kFrameCount] + capacity in ELEMENTS + Valid() + Reset(). This is now the fourth instance of the pattern (PathTracer's five, RTAcceleration's TLAS instance buffers, DebugOverlay's uploads), so lift it into a shared src/render/frame_upload_buffer.h/.cpp - the struct, EnsureFrameUploadBuffer, and CreateMappedUploadBuffer - and have Renderer use it. Leave PathTracer and RTAcceleration on their private copies for now; converting them is a separate, mechanical follow-up and folding it in here widens the blast radius of a change that is already large.

Why instancing is sufficient and fencing is not an alternative: D3D12Device::WaitForCurrentFrame waits on m_fenceValues[m_frameIndex] only - the last frame that used THIS back-buffer index, kFrameCount-1 frames back. It does not wait for the two frames after that. A single shared buffer would therefore be rewritten while two earlier frames are still reading it. Per-slot instancing is exactly what makes that narrow wait sufficient.

Growth follows EnsureFrameUploadBuffer's shape exactly: early-out when capacity suffices; newCapacity = elementCount + 64 headroom so growth amortises; allocate ALL kFrameCount replacements FIRST and Reset()+return false on any failure, so the live buffers are never left half-swapped; only then per slot Unmap old, null the mapped pointer, device.DeferredRelease(old), move the replacement in. Never drop the old ComPtr directly - a recorded command list may still reference it, and DeferredRelease pushes at m_globalFenceValue+1 which is the correct park point for work recorded this frame.

The frame slot is cached ONCE per frame into m_frameSlot, in BeginFrameResources, and nothing re-queries FrameIndex() mid-frame. That matches PathTracer::Dispatch (caches at the top) and RTAcceleration::BuildTLAS. It matters more here than there, because the shadow pass and the main pass are separated by BeginScenePass and BeginFrame and must not be able to land on different slots.

BeginFrameResources is what makes the slot correct, and writing it is non-optional:

  void Renderer::BeginFrameResources(D3D12Device& device)
  {
      m_frameSlot    = device.FrameIndex();
      m_currentFrame = m_frameSlot;   // the CB ring uses the same slot
      m_cbOffset     = 0;
      m_cbPeak       = 0;             // instrumentation, reset per frame
      m_drawItems.clear();
      m_batches.clear();
  }

BeginFrame drops its own `m_currentFrame = device.FrameIndex(); m_cbOffset = 0;` and gains a comment stating that BeginFrameResources owns frame-slot state and that BeginFrame is NOT the first thing in a frame - app.cpp:1104-1109 running the shadow pass at line 1106 versus BeginFrame at 1131 is the proof. Call BeginFrameResources at app.cpp:1066, right after ReclaimTextureDescriptors and ABOVE the raster/RT branch, so path-traced frames advance the slot too and an F1 toggle back to raster does not resume at a stale offset.

This closes a live bug, not just a hypothetical: today frame N's shadow constants are written into buffer[(N-1)%3] beyond that buffer's main-pass high-water mark, BeginFrame then rewinds a DIFFERENT buffer to 0, and the slot frame N's shadow draws are reading is reclaimed by frame N+2 after a wait that only covers frame N-1. On the very first frame both indices and offsets are 0, so the main pass overwrites the shadow constants at the same addresses before the GPU executes anything.

Zero-address guard: UploadCB currently returns 0 on overflow and every caller binds it straight into SetGraphicsRootConstantBufferView with no check, which is why the failure mode is silent GPU-address-zero reads rather than a clean abort. In the new code, if a CBPerPass upload fails or the batch list is empty, skip the pass entirely and never bind address 0. Keep UploadCB's error log and add peak tracking to it.

**shaderChanges**

New shaders/object_common.hlsli, included by basic_vs.hlsl and shadow_vs.hlsl so the two cannot drift (precedent: brdf_common.hlsli, display_common.hlsli):

  struct ObjectRecord
  {
      float4 worldT0; float4 worldT1; float4 worldT2;      // transposed object-to-world
      float4 normalT0; float4 normalT1; float4 normalT2;   // transposed InverseTranspose3x3
  };
  StructuredBuffer<ObjectRecord> g_Objects : register(t0, space2);
  cbuffer CBPerPass  : register(b3) { float4x4 viewProj; };
  cbuffer CBDrawBase : register(b0) { uint drawBase; };

  float3 DawningObjectWorldPos(ObjectRecord obj, float3 objectPos)
  {
      float4 p = float4(objectPos, 1.0);
      return float3(dot(obj.worldT0, p), dot(obj.worldT1, p), dot(obj.worldT2, p));
  }

basic_vs.hlsl: delete cbuffer CBPerObject (b0), include the .hlsli, take `uint instanceID : SV_InstanceID` as a second parameter, compute `uint index = drawBase + instanceID;` and `ObjectRecord obj = g_Objects[index];`. positionWS = DawningObjectWorldPos(obj, input.position); positionCS = mul(viewProj, float4(positionWS, 1.0)); normalWS = normalize(float3(dot(obj.normalT0.xyz, input.normal), dot(obj.normalT1.xyz, input.normal), dot(obj.normalT2.xyz, input.normal))) - the exact idiom path_trace.hlsl:635-637 already uses. VSOutput gains `nointerpolation uint drawIndex : TEXCOORD3;` set to index.

Per-vertex cost in the main pass is UNCHANGED, in fact slightly cheaper: today it does mul(worldViewProj,p) AND mul(world,p) for positionWS = two mat4-vec4. After: three dots plus one mat4-vec4.

shadow_vs.hlsl: same .hlsli, same SV_InstanceID parameter, returns mul(viewProj, float4(DawningObjectWorldPos(obj, input.position), 1.0)) - where viewProj is the LIGHT matrix this pass. It goes from one mat4-vec4 to three dots plus one mat4-vec4; one extra mat-vec on a depth-only pass is negligible. The file's header comment survives intact and gets stronger: instead of "both passes interpret b0 the same way", it becomes "both passes read the same bytes", which is unfalsifiable rather than merely conventional. The struct declares normalT0..2 that shadow_vs never reads - FXC dead-strips the loads but the declared struct is what sets the stride, so the declaration must stay identical, which is exactly why it lives in the shared .hlsli.

basic_ps.hlsl: delete `cbuffer CBMaterial : register(b2)` entirely, add

  struct MaterialRecord { /* the same 20 fields, same order, same offsets */ };
  StructuredBuffer<MaterialRecord> g_Materials : register(t0, space3);

PSInput gains the matching `nointerpolation uint drawIndex : TEXCOORD3;`. main() starts with `MaterialRecord mat = g_Materials[input.drawIndex];` and every bare albedo/roughness/metallic/useAlbedoTexture/albedoTextureIndex/... becomes mat.-qualified. Nothing else moves. ComputeShadow is untouched - it reads only b1 and the shadow map, so its deliberate single-exit shape (the X4000 workaround documented at lines 95-98) stays verbatim. b1/CBPerFrame keeps its full 176-byte declaration.

FXC vs_5_1 / ps_5_1 with /WX, checked against the constraints:
- StructuredBuffer in a vs_5_1 shader is legal; SRVs have been available in VS since SM 5.0.
- SV_InstanceID as a second VS parameter alongside a struct input is legal at 5.1.
- `nointerpolation uint` interpolant is legal at SM 4+.
- A 32BIT_CONSTANTS root parameter is spelled as an ordinary cbuffer in HLSL at 5.1; nothing special required.
- Interpolant budget: SV_POSITION plus positionWS(3), normalWS(3), color(4), uv(2), drawIndex(1) packs into 5 of 32 registers.
- No new return-inside-a-branch anywhere, so no X4000.
- Every VSOutput member is written on the single path, so no X3578 "output not completely initialized".
- The MAX_RASTER_TEXTURES /D plumbing at renderer.cpp:942-949 must survive unchanged; do not reintroduce a literal.

Also worth fixing in passing since basic_ps.hlsl is open: the comment at line 22 says CBPerFrame is "static_assert'd at 112 bytes" when the assert is 176 (112 is sky_ps.hlsl's deliberate prefix). AGENT_HANDOFF_CLAUDE.md:571 already lists it. And ComputeShadow hardcodes shadowExtent 24.0 / shadowMapSize 2048.0 as literals duplicating kShadowExtent and kShadowMapSize - not touched by this change, but they should be fed in as defines the way MAX_RASTER_TEXTURES is.

**shadowPassHandling**

The shadow pass's second copy of per-object data is DELETED, not shrunk. It stops existing as a concept.

It only exists today because CBPerObject stores a pre-multiplied worldViewProj, so the shadow pass has to re-upload the whole 192-byte struct just to substitute the light matrix - and DrawMeshShadow deliberately fills world and worldInvTranspose too (renderer.cpp:1787-1797), with a comment explaining that leaving stale bytes in a mapped ring is how a later shader change becomes a heisenbug. That reasoning is exactly the argument for one shared record: move the view-projection out of the per-object data into a per-PASS cbuffer, and the shadow pass has nothing per-object left to upload.

Concretely:
- Renderer::DrawMeshShadow is deleted.
- Scene::RenderShadowCasters is deleted (scene.cpp:146-171).
- Renderer::RenderShadowBatches(D3D12Device&) replaces both: it walks m_batches, and per batch sets the drawBase root constant, binds VB/IB, and issues DrawIndexedInstanced(mesh.indexCount, batch.instanceCount, 0, 0, 0).
- BeginShadowPass gains three binds after its existing SetGraphicsRootSignature/SetPipelineState: root param 6 (CBPerPass b3) uploaded with m_lightViewProj, root param 5 (object records root SRV at m_objectBuffer.buffer[m_frameSlot]->GetGPUVirtualAddress()), and nothing else. It still calls no SetDescriptorHeaps and still leaves params 1, 2, 3 and 4 unbound - legal exactly as today, because the shadow PSO has no pixel shader and all four are PIXEL-visible.
- BeginFrame does the symmetric thing for the main pass: same param 5 bind, same param 6 bind but with m_viewProj, plus the new param 2 material root SRV alongside its existing param 1 / 3 / 4 binds.

Per-entity shadow cost goes from 256 bytes of ring to zero. Per-PASS cost goes from zero to one 256-byte CBPerPass upload. Frame total for the shadow pass is 256 bytes regardless of entity count.

Cascades, which is the stated next step (CLAUDE.md:108-113) and the thing that halved the old ceiling to ~170: each additional cascade costs one more CBPerPass upload (256 bytes) and one more pass over m_batches. It does NOT multiply per-entity cost, because the object records are cascade-independent - the cascade only changes the matrix in b3. A four-cascade shadow pass costs 1 KB per frame instead of 768 bytes per entity per cascade. That is the single biggest structural win in the change, and it is the one the roadmap actually needs.

Two secondary fixes come free. RenderShadowCasters required a Material component (scene.cpp:160) even though the shadow pass never reads material data, so an entity without a Material silently cast no shadow; with one walk there is one filter. And the "draw index i means the same entity in both passes" property, which the whole indexing scheme depends on and which is currently an unenforced coincidence between two independently-written loops, becomes structural - there is literally one list.

The one thing to watch: both passes currently draw the identical set. If the main pass later gets frustum culling that the shadow pass does not share, the contiguity that SV_InstanceID addressing relies on breaks. The escape hatch is the StructuredBuffer<uint> indirection described under indexingMechanism - 4 bytes per visible draw per pass, and it is the shape GPU culling wants anyway.

**newCeiling**

BEFORE: ring is 262144 bytes; CBPerFrame takes 256 and each shadowed entity takes 768 (256 CBPerObject + 256 CBMaterial + 256 shadow copy, each rounded to D3D12's 256-byte CBV alignment). (262144 - 256) / 768 = 340.9, i.e. ~341 entities, halved to ~170 by a four-cascade shadow pass. Measured 5% at 17 entities, 29% at 97.

AFTER, the ring: CBPerFrame 256 + CBPerPass(shadow) 256 + CBPerPass(main) 256 = 768 bytes per frame, FLAT, independent of entity count. That is 0.29% of the 256 KB ring at any scene size. The ring stops being a scaling limit at all. Each additional shadow cascade adds 256 bytes, not 256 bytes times entity count - so the cascade halving disappears too.

AFTER, the per-entity cost: it moves to the structured buffers at 96 (object) + 80 (material) = 176 bytes per entity per frame, in buffers that GROW on demand via EnsureFrameUploadBuffer rather than living in a fixed ring. So there is no fixed ceiling left to quote; the cost is 176 x N x kFrameCount bytes of UPLOAD memory:
  1,000 entities  -> 528 KB across all three frames in flight
  10,000 entities -> 5.28 MB
  100,000         -> 52.8 MB
A 2,000-draw station interior is 352 KB per frame, 1.06 MB total. The binding constraint becomes upload-heap memory, which is measured in gigabytes.

For a like-for-like number against the old one - if you artificially capped the new data at the same 256 KB per frame: 262144 / 176 = 1,489 entities. And unlike the old figure, four cascades do NOT halve it, because the shadow pass no longer uploads anything per entity; it stays 1,489. So even under an artificial fixed cap the improvement is 1,489 versus 170, i.e. 8.8x. Unconstrained it is bounded only by memory.

Per-entity byte reduction: 768 -> 176, a 4.36x cut. Broken out: the shadow copy goes 256 -> 0 (deleted), the object record goes 256 -> 96 (192 bytes of real data that was being padded to 256, minus the 64-byte pre-multiplied worldViewProj that moved to the per-pass cbuffer, minus the fourth matrix row that is always (0,0,0,1)), and the material record goes 256 -> 80 (80 bytes of real data that was 68% alignment waste).

DRAW CALLS, measured on the real scenes in this tree. app.cpp:441-443 registers exactly three meshes: cube, plane, sphere. The demo block at app.cpp:466-508 creates GroundPlane(plane, groundTextures), BlueCube(cube, cubeTextures), RedSphere and GoldSphere (sphere, no textures), and seven SmallCubes (cube, no textures). Batching on (mesh, texture set) gives 4 batches. The growth test adds 80 cubes all sharing m_smokeGrowthMesh=cube and m_smokeGrowthMaterial=cubeTextures, which merge into the BlueCube batch - still 4 batches.
  Demo:   11 entities, 22 draws (11 main + 11 shadow) -> 8 draws.
  Growth: 91 entities, 182 draws -> 8 draws. 23x reduction.
Note RedSphere and GoldSphere batch together despite differing in albedo colour, roughness and metallic - scalar material fields are per-instance data, not batch state.

**bindlessAlignment**

Strongly toward SM 6.6, with nothing that has to be undone.

1. Root SRVs over StructuredBuffers is precisely what the DXR path already does - rt_pipeline.cpp:86-115 declares five of them, path_tracer.cpp:715-727 binds them - and CLAUDE.md:31-33 documents that as the deliberate pre-bindless choice ("This is a root SRV, not SM 6.6 bindless - no ResourceDescriptorHeap anywhere in the project"). The raster path adopting it makes the two paths architecturally consistent instead of introducing a third style. Root SRVs remain fully legal and idiomatic under SM 6.6.

2. Per-draw indexed records ARE the bindless data model. Going to SM 6.6 changes only where the texture DESCRIPTOR comes from: `materialTextures[mat.albedoTextureIndex]` becomes `ResourceDescriptorHeap[mat.albedoTextureIndex]`. The record layouts, the indexing scheme, the batching, the buffers, the root signature's SRV slots and the frame-instancing all survive untouched. Root param 3 (the 128-entry table) and the kMaxRasterTextures plumbing are what disappear - and that is the thing bindless is FOR, since ASSET_PIPELINE_SPEC.md:125-128 flags 128 slots as ~32 PBR materials that real content exhausts immediately.

3. Bindless makes this design STRICTLY BETTER rather than obsoleting it. The texture-index component of the batch key exists solely because SM 5.1 has no NonUniformResourceIndex. Under SM 6.0+ you wrap the index, drop those four fields from the key, and batching collapses to mesh-only - so every batch gets larger and the draw-call win grows. The migration deletes complexity from this design; it does not rewrite it.

4. It positions ExecuteIndirect and GPU culling. drawBase plus a StructuredBuffer<uint> indirection is one small step from an indirect argument buffer with GPU-written instance counts. The per-draw record buffer is the thing a compute culling pass would write into.

5. Budget direction is right. It consumes ZERO descriptor-heap slots - the scarce resource at 126 usable - and 3 more DWORDs of the abundant one, leaving 53 of 64 free. A descriptor-table variant would have spent from the scarce budget and required per-frame descriptors with the volatile-range execution-time hazard descriptor_allocator.h:14-25 exists to prevent.

The one thing it does NOT advance: this is still FXC vs_5_1/ps_5_1. It does not begin the DXC migration for the raster path, and it deliberately does not try to - CLAUDE.md:108 lists SM 6.6 bindless as the outstanding Layer 4 item, and doing the data-layout half now under FXC de-risks the compiler half later by ensuring only one variable changes at a time.

Test plan:
- STAGE 0, BEFORE ANY OTHER CHANGE - build the missing instrumentation the brief assumes exists. Grep confirms there is no Renderer::BeginFrameResources, no cb_ring_peak marker, and no 75% gate in tools/smoke_test.ps1; the only CB-ring instrumentation is the overflow error at renderer.cpp:1237-1239, which fires at 100% after draws are already reading GPU address zero. Add peak tracking inside UploadCB (m_cbPeak = max(m_cbPeak, m_cbOffset)), emit [SMOKE] cb_ring_peak=<bytes> cb_ring_pct=<pct> at shutdown, and add the 75% threshold to the harness. Run both smoke modes and record the numbers - expect roughly the documented 5% at the demo scene and 29% with the growth entities. Without this baseline there is no evidence the fix worked, only an argument that it should have.
- Build clean with the documented command, /WX enforced, zero warnings. Shader compilation is the first real gate: StructuredBuffer in vs_5_1, SV_InstanceID as a second VS parameter, and the nointerpolation uint interpolant all have to survive FXC before anything else is worth testing.
- Force the v1.0 root-signature branch once with a temporary edit that pretends CheckFeatureSupport failed, confirm it serializes and the app renders, then revert. The two branches at renderer.cpp:806 and :870 are independent copies and no local run exercises the fallback - a parameter added to only one produces a mismatch that surfaces on someone else's driver.
- Unit tests (TheDawningTests.exe) on the new pure-CPU draw_batcher, which has no D3D12 dependency for exactly this reason, the same way descriptor_allocator.h is structured: every draw item appears in exactly one batch; the batches partition [0, N) contiguously with ascending firstInstance; sum of instanceCount equals N; each item's record index equals its position in the upload order; items differing only in scalar material fields land in the SAME batch; items differing in any of the four texture indices land in DIFFERENT batches; items differing in mesh land in different batches. This catches the base/offset arithmetic without a GPU.
- Both smoke modes pass: tools/smoke_test.ps1 -RasterOnly and the full run. The DXR run needs dxcompiler.dll and dxil.dll present in build/Debug.
- New marker [SMOKE] cb_ring_peak - assert it is now 768 bytes flat and under 1%, with the growth entities live. This is the direct proof of the fix: the number must be identical at 17 entities and at 97, because it no longer depends on entity count.
- New markers [SMOKE] draw_batches=<count> draw_records=<count> instanced_max=<largest instanceCount>. With growth entities live assert draw_batches == 4, draw_records == entity count, and instanced_max >= 80. instanced_max is the guard that a later regression silently reverting to one-draw-per-entity fails loudly instead of merely running slower.
- Visual A/B against the DXR path via F1 at a fixed camera - the spec's stated oracle. This is what catches a transposed world or normal matrix, which is the second-most-likely silent failure. Critically it MUST be checked on a ROTATED entity: a transposed world matrix under an identity-rotation TRS looks completely correct. BlueCube and the seven SmallCubes are spinners (CreateSpinner), so use those, not GroundPlane.
- Shadow alignment: shadows stay attached to their casters and do not shear or detach. A world-matrix transpose confined to the shadow path detaches them; a normal-matrix transpose leaves shadows correct but lighting wrong, so both checks are needed and they are not redundant.
- Texture correctness under batching, both directions. Positive: the 80 growth cubes share a texture set with BlueCube and must sample the right albedo/normal/ORM/emissive - this is the NonUniformResourceIndex guard doing its job. Negative: temporarily give two entities in one mesh group DIFFERENT texture sets and confirm the batcher splits them into two batches rather than merging and sampling wrong.
- Ordering regression check on the app.cpp restructure: confirm BeginScenePass still leaves the scene pass pointing at the swap-chain-sized HDR target and not the 2048x2048 shadow depth target. app.cpp:1101-1103 warns about exactly this failure, and moving the gather above the shadow pass is the kind of edit that reintroduces it.
- F1 toggle stress: raster -> path tracing -> raster, several times, and confirm the first raster frame after each toggle is correct. BeginFrameResources is called above the branch specifically so the frame slot advances during RT frames; if it were inside the raster branch the first frame back would read a stale slot, which is the current behaviour and is easy to reproduce by mistake.
- Resize, which re-reads FrameIndex from the swap chain (d3d12_device.cpp:774, 784) rather than incrementing a counter - confirm the buffers and the cached m_frameSlot survive a resize without a stale-slot frame.
- Growth-then-shrink: spawn the 80 growth entities, destroy them, spawn again. Exercises EnsureFrameUploadBuffer's grow path and confirms it does not grow unboundedly on repeated cycles (capacity is sticky by design; assert it stops growing after the first cycle).

Negative test: The failure this design is most likely to hit is silent, and it is the one the survey correctly identified as the single most probable way the intended fix fails quietly: the per-draw index resolving to 0 for every instance. Either SV_InstanceID is misused (someone "helpfully" routes it through StartInstanceLocation) or drawBase is never set / set once outside the batch loop. The symptom is that every instance in a batch renders as the FIRST instance - same transform, same material - so 81 cubes stack on top of one cube. That reads as "objects vanished", which is easy to misdiagnose as a culling or upload bug, and with 1-instance batches (the demo scene before growth entities exist) it does not reproduce at all.

THE TEST: build with `drawBase` forced to 0 in RenderBatches and RenderShadowBatches, run the full smoke suite, and require that it FAILS. If it passes, the suite does not actually test the thing this whole design rests on and the thresholds must be tightened until it does.

Three independent assertions should each catch it, and all three must be verified to actually trip under the forced-zero build - an assertion that does not fail on a known-broken build is decoration:

1. Shadow coverage. Renderer::ReadShadowMapCoverage already reports the fraction of probe texels below the 1.0 clear value, and the growth test spawns 80 cubes at distinct transforms. Collapsing them onto instance 0 collapses their shadow footprint to a single cube's. Emit [SMOKE] shadow_written_fraction with the growth entities live and assert a floor calibrated between the correct value and the collapsed one. This is the strongest check because it uses an oracle that already exists, is already GPU-side, and cannot be satisfied by CPU-side bookkeeping.

2. Back-buffer capture. The existing capture path compares rendered output; 81 cubes collapsed to one changes the frame substantially. Weaker than (1) because it depends on capture thresholds, but it is a second, independent signal.

3. Batcher unit test on the CPU side. Asserting that firstInstance is ascending and that batches partition [0, N) catches drawBase being computed wrongly at build time. It does NOT catch drawBase being computed correctly and then not bound, which is why (1) is required and (3) alone is insufficient.

A second, separate negative test for the other high-risk silent failure - the structured-buffer stride. Because a root SRV takes a bare GPU virtual address with no StructureByteStride and no descriptor, nothing in D3D12 or the debug layer cross-checks the shader's declared struct against the C++ sizeof. Deliberately add a float to GPUObjectRecord without adding it to object_common.hlsli, confirm the static_assert catches it at compile time, and then confirm what happens if you defeat the static_assert too: the render must visibly break rather than degrade subtly. If it degrades subtly, that is an argument for adding a runtime check - a known sentinel value in a reserved field of record 0, read back once at startup.

Third, for the transpose risk that mesh.h:76-78 says this tree already shipped once: flip the transpose in the CPU-side record fill (write m[row][col] instead of m[col][row]) and confirm the DXR A/B on a spinning cube shows an obvious disagreement. If it does not - because the check was done on an axis-aligned unrotated object - the A/B procedure is wrong and must be redone on a spinner.

Risks: SV_InstanceID / drawBase silent zero. Every instance in a batch renders as the first one. Compiles, runs, no debug-layer error, and does not reproduce at all when every batch has one instance - which is the state of the demo scene before growth entities exist. This is the design's central assumption and it must be actively falsified, not assumed. Covered by the forced-drawBase-zero negative test. | Matrix transpose in a StructuredBuffer. render/mesh.h:76-78 records that this tree already made this exact mistake once, on RTInstanceData::normalMatrix, and that it made the raster and DXR paths disagree on the same geometry. Mitigated structurally by storing explicit float4 rows and never a float4x4 inside a structured buffer, and by A/B-ing against DXR on a ROTATED entity - an identity-rotation TRS looks correct either way, so an A/B on GroundPlane proves nothing. | NonUniformResourceIndex does not exist at SM 5.1. materialTextures[mat.albedoTextureIndex] must be wave-uniform, which is free today because the index comes from a root CBV constant across the draw, and which batching is exactly what breaks. Handled by putting the four texture indices in the batch key. If anyone later widens the key for bigger batches, the result is wrong textures on some pixels, hardware-dependently, possibly not reproducing on the dev GPU at all. The fix is SM 6.0+, not a workaround. | Structured-buffer stride is unvalidated by anything. A root SRV takes a bare GPU virtual address - no StructureByteStride parameter, no descriptor, nothing for the debug layer to cross-check. The shader's declared struct is the sole definition of the stride, and a C++/HLSL disagreement reads garbage silently. The .hlsli-as-single-HLSL-definition discipline and the static_asserts are the only guards, and both are conventions rather than enforcement. | BeginFrameResources is a prerequisite that does not exist in this tree. If someone builds the structured buffers without hoisting the frame-slot advance above the shadow pass, the shadow pass reads the previous frame's buffer slot and the change silently inherits the very bug it was supposed to close - now with per-object transforms instead of just constants, so the failure is a whole frame of stale geometry rather than stale lighting. Anyone planning against the brief's assumption that this call already exists is planning against a tree that is not here. | app.cpp pass restructure. Moving the gather above the shadow pass reorders BeginScenePass, BeginFrame, the viewport/scissor set and draw submission relative to each other. app.cpp:1101-1103 explicitly warns that running the shadow pass after BeginScenePass leaves the scene pass pointing at the 2048x2048 depth-only target. This is the highest-churn, lowest-glamour part of the change and the easiest place to introduce a regression that only shows as a black frame. | Per-frame CPU sort is new work on the critical path, before the shadow pass. O(N log N) on a small key POD - well under a millisecond at 10k entities, but not free, and it did not exist before. Escape hatches if it ever matters: bucket by mesh-handle index into a handle-indexed table for O(N), or cache the ordering and re-sort only when the MeshInstance pool version changes. | Draw order is no longer pool order. Nothing today depends on it (opaque geometry, depth-tested, a single PSO), but transparency will, and it will need its own back-to-front path outside this batcher rather than an attempt to make the batcher order-preserving. | No frame-to-frame index stability. Records are rebuilt every frame and the sort order shifts whenever the sparse-set pool compacts on entity add/remove. Correct today, but anything later that wants a persistent per-object identity - TAA motion vectors, GPU culling, object picking - needs a stable ID stored IN the record rather than derived from the draw index. Adding a uint entityId field now costs 4 bytes and nothing else; retrofitting it later means touching the layout, both shaders and the batcher again. | Affine-only assumption in the three-row world matrix. Holds for Transform::ToCameraRelativeMatrix today and matches what D3D12_RAYTRACING_INSTANCE_DESC::Transform already assumes engine-wide, so it is consistent rather than novel. But a projective per-object transform would break silently and no static_assert can catch it - it needs a header comment and a code review habit. | Contiguity coupling between the two passes. One buffer and one batch list serve both passes only because they draw the identical set. Frustum culling on the main pass alone breaks it. The StructuredBuffer<uint> indirection is the designed-in escape hatch, but leaving it out now means a future culling change is a shader edit, not a pure CPU change. | Scope. This touches the root signature (both branches), three shaders plus a new shared .hlsli, the renderer's frame model, the scene's render entry points, and the frame structure in app.cpp - and it lands on top of a live latent race it is also fixing. Landing it as one commit makes bisecting a regression hard. Sequencing it as (1) instrumentation, (2) BeginFrameResources and the ring-ownership fix alone, (3) structured buffers with instanceCount always 1, (4) batching - keeps each step independently verifiable, and step 3 already captures the entire ring win with the SV_InstanceID risk still dormant.

## Judge lens: winner = Design 1

See the reasoning field above.

Fatal flaws:
- BOTH designs verified against the wrong tree. Every file:line citation in both is from D:/The Dawning (new)/The Dawning, not the worktree D:/The Dawning (new)/.agents/worktrees/claude-cascades. Line numbers cited for renderer.cpp, app.cpp and smoke_test.ps1 will not match the tree the work lands in.
- BOTH designs claim Renderer::BeginFrameResources does not exist and must be written, and present hoisting the frame-slot advance above the shadow pass as a live bug fix. It exists at renderer.cpp:1293 and is already called unconditionally at app.cpp:1115. Design 1 calls this 'not optional polish'; Design 2 calls it 'a prerequisite that does not exist in this tree'. Both are false.
- BOTH designs claim there is no [SMOKE] cb_ring_peak marker and no 75% gate in smoke_test.ps1. The marker is emitted at app.cpp:748, m_cbPeak is tracked in UploadCB at renderer.cpp:1272, ConstantRingPeakBytes()/ConstantRingCapacity() are at renderer.h:233-234, and the 75% throw is at smoke_test.ps1:157-165. Design 2's entire Stage 0 is rebuilding work already done.
- Design 2 miscounts the demo scene: it reports 11 entities and 91 with growth, having read only app.cpp:466-508 and missed the six Pillars created in the loop at app.cpp:520-536. Truth is 17 and 97. It then proposes exact-value smoke assertions (draw_records == entity count) against those wrong numbers.
- Design 1 places a projective float4x4 worldViewProj inside a StructuredBuffer relying on a column_major annotation, directly against the in-tree doctrine at mesh.h:75-78 which states StructuredBuffer elements do not get the cbuffer column-major reinterpretation and records that this exact bug already shipped once on RTInstanceData::normalMatrix. This is the highest-consequence silent-failure path in Design 1 and its mitigation is an annotation, not a structure.
- Design 2's central indexing risk (drawBase/SV_InstanceID resolving to 0) is invisible in the 17-entity demo scene where several batches have one instance, and its NonUniformResourceIndex batch-key constraint produces hardware-dependent wrong-texture failures that may not reproduce on the dev GPU. Neither is covered by an oracle that fails deterministically.
- Design 2 forfeits pixel identity as an acceptance criterion by changing draw order, VS math (premultiplied worldViewProj to mul(viewProj, worldPos)) and shadow-caster filtering, leaving its strongest correctness check dependent on calibrated thresholds and visual A/B.
- Design 2 claims collapsing the two pool walks fixes scene.cpp:160 where an entity without a Material 'silently casts no shadow'. Verified: Scene::RenderEntities applies the identical HasByIndex<ecs::Material> filter, so the two loops already agree and nothing is silently excluded. Either the single walk keeps the filter and fixes nothing, or it relaxes it and changes main-pass behavior. Design 1 read this correctly as harmless.

Required corrections:
- Re-verify every file:line citation against D:/The Dawning (new)/.agents/worktrees/claude-cascades before writing any code. The main tree is stale and both designs' line numbers derive from it.
- Delete the BeginFrameResources creation work and the entire frame-ordering bug-fix narrative. The function, its call site above the raster/RT branch, and BeginFrame's matching comment already exist. Scope the change to ADDING a maxDrawsHint parameter plus m_objectCursor/m_materialCursor resets and the EnsureFrameStructuredBuffer calls to the existing body.
- Delete Design 2's Stage 0 instrumentation work. cb_ring_peak, cb_ring_capacity, m_cbPeak tracking in UploadCB and the 75% smoke gate all exist. Record the current baseline numbers from a pre-change run instead of building the harness.
- Adopt Design 2's matrix representation into Design 1: move viewProj out of the per-object record into a small per-pass cbuffer (b3) uploaded once per pass with m_lightViewProj in BeginShadowPass and m_viewProj in BeginFrame, and store only affine world and normalMatrix as explicit float4 rows (12 floats each, 96 bytes) consumed by dot products. This obeys mesh.h:75-78 and path_trace.hlsl:635-637, removes the float4x4-in-StructuredBuffer transpose risk entirely, and makes each additional shadow cascade cost 256 bytes flat rather than 192 bytes per entity per cascade. Note that the per-vertex clip-space math changes, so pixel identity becomes near-identity; keep the capture diff but state a small float tolerance.
- Keep Design 1's scope boundaries: no batching, no SV_InstanceID, no draw-order change, no deletion of DrawMeshShadow or Scene::RenderShadowCasters, no new shared frame_upload_buffer.h/.cpp, no CMakeLists change. Copy the FrameUploadBuffer shape into the Renderer as a private struct exactly as PathTracer, RTAcceleration and DebugOverlay each do.
- Keep the object cursor's disjoint per-pass ranges (shadow [0,N), main [N,2N)) from Design 1 rather than Design 2's shared single record, so a future divergence in the two loops' filters costs buffer growth rather than wrong transforms.
- Import Design 2's negative-testing rigor: build with the per-draw index forced to 0, run both smoke modes, and require FAILURE. Verify each assertion actually trips on that broken build before declaring it a test. Calibrate the shadow_written_fraction floor between the correct and collapsed values rather than asserting a magic constant.
- Add the shadow_records == main_records parity marker and assertion, and assert cb_ring_pct falls to ~0 after the change. Retain the existing 75% gate rather than replacing it.
- Verify the matrix and normal handling by F1 A/B against the DXR path on a ROTATED, NON-UNIFORMLY SCALED entity. Do not use GroundPlane or any identity-rotation TRS, which looks correct under a transpose. Use BlueCube or a SmallCube spinner.
- Edit both root-signature branches in lockstep (the v1.1 and v1.0 copies in CreateRootSignature) and confirm by side-by-side diff; the fallback cannot be exercised locally. Fix the two stale DWORD strings (header comment claiming 7, success log claiming 9; truth is 8 today) and the static-sampler ordering defect where staticSamplers[] is built before staticSampler.RegisterSpace/.ShaderVisibility are assigned, but keep those in a separate commit from the buffer change.
- Add static_assert on sizeof and offsetof for every field of both new structs. A root SRV has no descriptor and therefore no StructureByteStride for the runtime to validate, so these asserts are the only guard against a C++/HLSL stride disagreement. CBPerObject currently has no size assert (CBMaterial at renderer.h:89 does); close that gap.
- Update ASSET_PIPELINE_SPEC.md's 'Known scaling limits' section in the same change, replacing the ~341/~170 figures with the new cost model and naming CPU draw submission and the 128-slot texture heap as the remaining constraints.

## Judge lens: winner = Design 1

Both designs are technically sound and both pass the four checks in my lens. I verified against the tree rather than taking either at its word.

ROOT SIGNATURE, recomputed independently. renderer.cpp:806-852 (v1.1) and :870-905 (v1.0) are two independent 5-param copies: CBV(2)+CBV(2)+CBV(2)+table(1)+table(1) = 8 DWORDs today. The comment at :717-723 says 7, the log at :924 says 9; both stale, both designs caught both. Design 1 after: 2(obj SRV)+2(b1 CBV)+2(mat SRV)+1+1+2(32BIT_CONSTANTS Num=2) = 10. Design 2 after: 1(32BIT Num=1)+2+2+1+1+2(obj SRV)+2(b3 CBV) = 11. Both arithmetically correct.

HLSL LEGALITY. Verified b3 is free across all raster shaders (only b0/b2 used in basic_vs/shadow_vs/basic_ps; bloom/overlay/tonemap b0 sit under separate root signatures). StructuredBuffer + SRVs in vs_5_1 are legal at SM 5.0+. Design 2's `nointerpolation uint drawIndex : TEXCOORD3` is legal at SM4+ and TEXCOORD3 is free (0/1/2 in use). Design 2's SV_InstanceID usage is legal AND safe: it never touches StartInstanceLocation, so the SM 5.1 trap both designs correctly identify cannot fire. Neither design introduces a return-inside-a-branch, so ComputeShadow's X4000 single-exit shape is untouched in both.

FRAMES-IN-FLIGHT. Both correctly reject fencing and barriers (d3d12_device.cpp:431 WaitForCurrentFrame waits only on m_fenceValues[m_frameIndex], i.e. kFrameCount-1 back; path_tracer.h:98-108 states barriers cannot order CPU writes) and both kFrameCount-instance following the FrameUploadBuffer house pattern. Both correct.

SHADOW PASS. BeginShadowPass (renderer.cpp:1594-1632) calls SetGraphicsRootSignature and SetPipelineState and never SetDescriptorHeaps — confirmed. Both designs correctly derive root SRVs over tables from this. Both correctly note params 1-4 stay unbound (already true today, proven by the shipping code, legal because the shadow PSO has no PS).

BOTH designs independently discovered that the task brief is wrong: `grep -rn "BeginFrameResources|cb_ring_peak|FrameResources" src/ tools/ shaders/` returns nothing. No BeginFrameResources, no cb_ring_peak, no 75% gate in smoke_test.ps1. Both correctly diagnose the live frame-slot bug (shadow pass at app.cpp:1104-1109, BeginFrame at :1131). That agreement, reached independently, is strong evidence both did real verification.

WHERE DESIGN 2 IS BETTER, and I want this on record because it is not close on these three points:
1. Matrix packing. mesh.h:69-82 is a first-person in-tree record that THIS TREE ALREADY SHIPPED a StructuredBuffer matrix-packing bug (RTInstanceData::normalMatrix), and lines 76-78 explicitly assert "StructuredBuffer elements do not get the column-major reinterpretation that cbuffer matrices do." Design 2's explicit float4 rows + dot() reconstruction follows that precedent and removes the question. Design 1 bets on `column_major` on a struct member being honored by FXC inside a StructuredBuffer element — against an in-tree comment that says the reinterpretation does not apply there.
2. Cascades. Design 1 pre-multiplies worldViewProj into the object record, so N cascades cost N object records per entity (Design 1's own number: 1,040 bytes/entity at 4 cascades). Design 2 moves viewProj to a per-pass b3 cbuffer, making records cascade-independent — 4 cascades cost 4x256 bytes of ring, flat. CLAUDE.md:108-113 names cascades as next. This is the better structural idea in either document.
3. Scene arithmetic. I verified app.cpp:466-508: 11 renderables over 3 meshes. Design 2's 4-batch decomposition (plane+groundTex / cube+cubeTex / sphere+none x2 / cube+none x7) and its instanced_max>=80 claim are exactly right, including that the 80 RTGrowth cubes merge into the BlueCube batch. Design 2 did more verification work.

WHY DESIGN 1 STILL WINS UNDER *THIS* LENS. The lens is will-it-work, and the deciding factor is exposure to silent, unvalidatable failure:

(a) NonUniformResourceIndex. FXC 5.1 has no such intrinsic. Today `materialTextures[albedoTextureIndex]` is uniform for free because the index comes from a per-draw root CBV. Design 1 preserves that exactly — a root constant, one draw call, uniform by construction, zero exposure. Design 2 makes wave-uniformity a property enforced ONLY by a CPU-side sort key. Design 2 is honest about this, but the failure mode is wrong textures on some pixels, hardware-dependently, with no debug-layer message and possibly no reproduction on the dev GPU. That is the worst failure signature available and it is newly introduced.

(b) Pixel identity as an oracle. Design 1 is a pure data relocation: same matrices, same math, same draw order, so byte-identical capture is a legitimate acceptance criterion — and it catches the transpose bug, the wrong-index bug and the field-offset bug simultaneously. Design 2 forfeits this twice over: it changes the vertex math from mul(worldViewProj,p) to mul(viewProj, mul(world,p)) (different rounding) and it reorders draws by sort key. Design 2's residual oracles (visual A/B, shadow coverage, markers) are all weaker. Under a will-it-work lens, giving up the strongest available test is a first-order cost.

(c) Blast radius. Design 2 adds draw_batcher.h/.cpp, frame_upload_buffer.h/.cpp, gpu_draw_records.h, object_common.hlsli, a CMakeLists edit, deletes DrawMeshShadow and Scene::RenderShadowCasters, rewrites the scene render entry points, and reorders app.cpp's pass structure — against the explicit warning at app.cpp:1101-1103 that mis-ordering leaves the scene pass bound to the 2048x2048 depth target. Design 1 changes zero draw logic in scene.cpp, keeps both DrawMesh/DrawMeshShadow signatures, and adds one call to app.cpp.

Design 2 concedes the argument itself: its own sequencing note says step 3 (structured buffers with instanceCount always 1) "already captures the entire ring win with the SV_InstanceID risk still dormant." Step 3 is Design 1. The stated problem is the constant-ring ceiling; Design 1 removes it entirely (ring drops to CBPerFrame alone, 256 bytes flat). Batching solves a different, explicitly-next problem — CPU draw submission — and Design 1 is a prerequisite for it, not an obstacle.

Correct call: ship Design 1, transplant Design 2's matrix-row storage and its per-pass-viewProj split, and keep batching as the follow-on Design 2 itself sequences it as.

Fatal flaws:
- Design 2 — NonUniformResourceIndex does not exist at FXC 5.1, and Design 2 converts wave-uniformity of `materialTextures[mat.albedoTextureIndex]` from a free structural guarantee into an invariant enforced only by the CPU-side batch sort key. Today the index comes from a per-draw root CBV and is uniform by construction. Under batching it is loaded from a StructuredBuffer via a nointerpolation interpolant that varies per instance within a wave; it is correct only because the four texture indices are in the key. Any future key widening produces wrong textures on some pixels, hardware-dependently, with no compile error and no debug-layer message. Design 2 identifies this honestly but it is a newly-introduced class of silent failure that Design 1 has zero exposure to.
- Design 2 — forfeits pixel identity as an acceptance criterion, twice. It replaces mul(worldViewProj, p) with mul(viewProj, mul(world, p)) (different float rounding) and it reorders draws by sort key. Design 1 is a pure data relocation and can require a byte-identical back-buffer capture, which catches the matrix-transpose failure, the wrong-index failure and the material-field-offset failure in a single test. Design 2's remaining oracles (visual A/B via F1, shadow_written_fraction, CPU batcher unit tests) are each strictly weaker and none of them covers all three.
- Design 1 — bets the highest-consequence risk in the change on `column_major` being honored by FXC on a struct member inside a StructuredBuffer element, directly against mesh.h:76-78, which is an in-tree record that this exact tree already shipped this exact bug on RTInstanceData::normalMatrix and which asserts that StructuredBuffer elements do NOT get the cbuffer column-major reinterpretation. If FXC silently ignores the modifier there, every transform in the engine breaks with no compile error and no validation message (core/types.h:15-43). Design 1's mitigation (pixel-identity capture) would catch it, but the design commits to the layout before proving the annotation does anything.
- Design 1 — pre-multiplies worldViewProj into the object record, so the shadow pass must write a full second 192-byte record per entity and each additional cascade adds another. Design 1's own figure is 1,040 bytes/entity/slot at four cascades. CLAUDE.md:108-113 names cascades as the next step. Design 2's split of viewProj into a per-pass b3 cbuffer makes records cascade-independent (4 cascades = 4x256 bytes of ring, total, flat) and deletes the shadow pass's per-object write entirely. This is a real structural weakness in the winner, not a nitpick.
- Design 2 — claims collapsing the two scene walks 'fixes scene.cpp:160, where an entity without a Material silently casts no shadow.' Verified against scene.cpp:146-171 and :173-230: RenderEntities carries the IDENTICAL Material filter, so such an entity is not drawn in either pass today — the behaviour is consistent, not buggy. Dropping the requirement makes those entities newly visible AND newly shadow-casting. That is a deliberate behaviour change presented as a bug fix.

Required corrections:
- Before committing to Design 1's ObjectData layout, PROVE the column_major annotation. Compile a throwaway HLSL declaring `struct S { column_major float4x4 m; }; StructuredBuffer<S> b : register(t0, space2);` with fxc /T vs_5_1 and disassemble the load, or run the tree's own mutation (flip column_major to row_major and confirm the capture changes enormously) BEFORE the main edit rather than after. mesh.h:76-78 states StructuredBuffer elements do not get the cbuffer reinterpretation; if that is right, the annotation is load-bearing in a path this tree has already been burned on. If the disassembly is ambiguous, fall back to Design 2's explicit float4 rows plus dot() reconstruction, which is the in-tree precedent and removes the question entirely.
- Transplant Design 2's per-pass viewProj split into Design 1. Store `world` and `worldInvTranspose` only in the object record and move the view-projection to a small CBPerPass cbuffer at b3 (m_lightViewProj in BeginShadowPass, m_viewProj in BeginFrame). This deletes the shadow pass's per-object write outright rather than merely relocating it, halves object storage, and makes records cascade-independent so a four-cascade upgrade costs 4x256 bytes of ring instead of 4 records per entity. Accept that this costs bit-identity in the capture (mul(viewProj, mul(world,p)) rounds differently) — decide that trade explicitly rather than inheriting it. If bit-identity is judged more valuable than cascade-readiness, keep pre-multiplication and document that cascades will need this change later.
- Design 1's spec never states where BeginFrame binds root params 0 and 2. It says only that BeginShadowPass 'gains exactly one line.' BeginFrame must gain SetGraphicsRootShaderResourceView for BOTH the object buffer (param 0) and the material buffer (param 2), before DrawSky, since DrawSky runs under the main root signature. Write this into the plan.
- Fix Design 1's claim that shadow_written_fraction 'is already read by the harness.' Verified: app.cpp:1196 emits it, but smoke_test.ps1:150 asserts only `shadow_map_written` = yes. The fraction is emitted and ignored. The threshold assertion must be ADDED, and it must be calibrated by first recording the baseline, then running the forced-index-zero mutation and confirming the assertion actually trips. An assertion that does not fail on a known-broken build is decoration.
- Build the missing instrumentation FIRST, as its own commit, before touching the buffers. Neither BeginFrameResources nor [SMOKE] cb_ring_peak nor a 75% gate exists (grep-verified). Add peak tracking in UploadCB, emit cb_ring_peak/cb_ring_pct alongside the existing markers near app.cpp:736, add the 75% threshold to smoke_test.ps1, and RECORD the pre-change numbers on both smoke modes. Expect ~5% on the demo scene and ~29% with the 80 growth entities live, matching ASSET_PIPELINE_SPEC.md:108-123. Without that baseline there is no evidence the fix worked, only an argument that it should have.
- Land the frame-slot fix (BeginFrameResources hoisted above the shadow pass, called unconditionally at app.cpp:1066 above the raster/RT branch) as a SEPARATE commit before the buffer change. It is a live correctness fix, not a refactor, and it is independently verifiable. Bundling it makes a regression bisect ambiguous, and reviewers expecting bit-identity on frame 0 will read the corrected shadow map as a regression.
- Edit both root-signature branches side by side and diff them manually. renderer.cpp:806-852 and :870-905 are independent copies and the v1.0 path only runs when CheckFeatureSupport reports no 1.1 support, so no local run — including both smoke modes — exercises it. Adding param 5 to only one branch produces a signature mismatch on someone else's driver. Consider a temporary forced-fallback build to confirm it serializes, then revert. Also correct the two stale strings in that function (:717-723 says 7, :924 says 9; truth is 8 today, 10 after).
- Keep the two adjacent pre-existing defects out of the buffer commit. renderer.cpp:773 builds staticSamplers[] BEFORE lines 774-775 assign staticSampler.RegisterSpace/.ShaderVisibility, so s0 ships as SHADER_VISIBILITY_ALL rather than PIXEL (verified — benign, since ALL is a superset and RegisterSpace 0 is the zero-init value, but the lines read as if they took effect). And basic_ps.hlsl:22 claims CBPerFrame is static_assert'd at 112 bytes when renderer.h:68 asserts 176 (112 is sky_ps.hlsl's deliberate prefix, which is the actual append-only constraint). Fix both, in their own commit.
- Add static_asserts on sizeof AND offsetof for every field of both new structs in renderer.h. A root SRV is a bare GPU VA with no descriptor and no StructureByteStride, so nothing in D3D12 or the debug layer cross-checks the shader's declared struct against the C++ one — a one-byte disagreement reads shifted garbage silently for every element after the first. CBPerObject currently has no size assert at all (verified: CBPerFrame and CBMaterial have them, CBPerObject does not); this closes that gap and it is the only guard that will exist.
- Assert shadow_records == main_records and both > 0 in smoke_test.ps1, rather than asserting a magic entity count. Verified that scene.cpp:146-171 and :173-230 carry identical visible/Transform/Material filters, which is what makes Design 1's 2 x maxDrawsHint object capacity sufficient — but that parity is enforced nowhere today. Assert the invariant so a future divergence (frustum culling, a castsShadow flag, LOD) surfaces as a test failure rather than a buffer overflow. Note the demo scene is 11 renderables, not the 17 the brief implies (17 is total EntityCount); size any count-based assertion off the renderable count.
