# The Dawning V3 — Engine Analysis Report

> **Historical snapshot:** This report describes an early V3 commit and is kept
> for provenance. Its source inventory, test/CI status, simulation status, and
> several defect findings are no longer current. Use the root `README.md` and
> `docs/research/RUNTIME_INTEGRATION_AUDIT_2026-07-21.md` for implemented status.

---

## 1. What this is

**The Dawning V3 is a competent D3D12/DXR renderer bring-up — roughly 10,078 lines of C++ and 916 lines of HLSL across 46 files — accompanied by ~9,400 lines of design documentation describing a multiplayer, planetary-scale, economy-driven space sim.** That ratio is the project's defining characteristic.

What actually runs: a Win32 window with raw mouse input, a QPC timer with a fixed-step accumulator, a printf logger, a hand-rolled header-only math library, a D3D12 device with triple buffering and DRED, a raster path (one root signature, two PSOs, Cook-Torrance GGX direct lighting, albedo + normal maps, gradient sky), a DXR 1.1 megakernel path tracer (BLAS pool, per-frame TLAS, SBT, NEE with shadow rays, Russian roulette), a sparse-set ECS, a generational slot-map ResourceManager, KTX/PNG/DDS texture loaders, a GDI debug overlay, and a `WinMain` that hardcodes 3 meshes, 4 textures, and **12 entities**.

`src/` contains exactly four directories: `core/`, `ecs/`, `render/`, `scene/`. Of the 13 pillars in `MASTER_ENGINE_SPEC.md`, pillars 4–13 (flight, character gameplay, ship interiors, world streaming, AI, economy, narrative, networking, tooling, production infra) have **no code at all** — not stubs, not headers, not empty directories.

**The honest characterization:** a strong learning project with real engine bones. Nothing in the live source is fake — every function does what its name says, the LH math is verifiably correct, the DXR pipeline genuinely dispatches rays. But the documentation describes an engine roughly one layer ahead of the code, there are **zero automated tests of any kind**, the project's own Rule 1 (double-precision world positions) is violated engine-wide, and a headline feature advertised in three documents does not exist.

Live source is **~494 KB**. Working tree is ~176 MB. That gap is scaffolding, not engine.

---

## 2. How it works

### Layering

Declared: `core → ecs → render → scene → main`. Mechanically it holds — `core/types.h` includes nothing from the engine, `ecs/` includes only `core/types.h`, `render/` includes `core/`, `scene/` includes all three. No cycles, no back-references. For a hand-rolled engine that is genuine discipline.

But two boundaries are drawn wrong:

- **`scene` is fused to `render`, not layered above it.** `scene.h` includes `renderer.h`, `path_tracer.h`, and `d3d12_device.h`, and holds a `render::PathTracer` **by value** (`scene.h:88`). The scene graph cannot be compiled or reasoned about without D3D12.
- **`main.cpp` is the real renderer.** `Renderer` does not own a frame. `main.cpp` opens/closes the command list, performs every back-buffer barrier, sets RTV/viewport/scissor, clears, and chooses the render path. `Renderer` is a PSO-and-constant-buffer helper named after a responsibility it doesn't have. 776 lines of `WinMain` contain the frame graph.
- **`D3D12Device` stops one layer too early.** It creates exactly one resource in its life — the depth buffer. No upload allocator, no descriptor allocator. `D3D12_HEAP_TYPE_UPLOAD` is hand-rolled in **seven files**, and three independent shader-visible CBV/SRV/UAV heaps exist (`renderer.cpp:399`, `path_tracer.cpp:177`, `debug_overlay.cpp:179`).

### Frame walkthrough — raster mode (default)

| Step | Location | What happens |
|---|---|---|
| Wait | `main.cpp:650` | `device.WaitForCurrentFrame()` blocks on `m_fenceValues[frameIndex]` — frame N−3's work |
| Reset | `main.cpp:651` | `ResetCommandList` resets this frame's allocator + the shared list |
| Barrier | `main.cpp:703` | Back buffer `PRESENT → RENDER_TARGET` |
| Clear | `main.cpp:709-711` | `ClearRenderTargetView` + `ClearDepthStencilView(1.0)` + `OMSetRenderTargets` |
| Viewport | `main.cpp:713-719` | viewport / scissor |
| BeginFrame | `main.cpp:721` | Caches frameIndex, resets `m_cbOffset = 0`, `SetDescriptorHeaps`, root sig, PSO, topology; computes viewProj; uploads `CBPerFrame` → root slot 1; binds SRV table → root slot 3 |
| Sky | `main.cpp:722` | Swaps to `m_skyPSO`, nulls VB/IB, `DrawInstanced(3,1,0,0)` (fullscreen triangle from `SV_VertexID`), restores `m_pso` |
| Geometry | `main.cpp:723` | `Scene::RenderEntities` iterates the `MeshInstance` sparse-set pool; per entity computes `wvp = world * viewProj` and `InverseTranspose3x3`, sub-allocates `CBPerObject` (192B → 256B) and `CBMaterial` (32B → 256B) into the ring, sets VB/IB, `DrawIndexedInstanced` |
| Overlay | `main.cpp:725-739` | Optional; rebinds its own RTV/heaps/rootsig/PSO |
| Barrier | `main.cpp:741` | `RENDER_TARGET → PRESENT` |
| Present | `main.cpp:746` | `Close`, `ExecuteCommandLists`, `Present(1)`, then `MoveToNextFrame` signals the fence, re-reads `GetCurrentBackBufferIndex()`, waits again |

### Frame walkthrough — path-tracing mode (F1)

Same wait/reset prologue, then:

| Step | Location | What happens |
|---|---|---|
| Barrier | `main.cpp:662` | `PRESENT → RENDER_TARGET` — purely to satisfy `CopyToBackBuffer`'s hardcoded `StateBefore`. The back buffer is never used as an RT here |
| TLAS | `main.cpp:667` | `BuildAccelerationStructures` rebuilds TLAS from ECS transforms every frame |
| Dispatch | `main.cpp:673` | `PathTraceEntities` flattens materials / instance metadata / triangle normals+UVs+positions into upload buffers, dedupes textures into descriptor tables; `path_tracer.cpp:678-732` binds compute root sig, RT state object, 9 global root params, `DispatchRays` |
| Barriers | `path_tracer.cpp:735-740` | Two UAV barriers on HDR history + LDR display textures |
| Copy | `main.cpp:676` | Paired transitions (display `UAV → COPY_SOURCE`, back buffer `RENDER_TARGET → COPY_DEST`), `CopyResource`, then back |
| Present | `main.cpp:746` | As above |
| **Stall** | `main.cpp:755-756` | `if (renderedPathTracing && !device.IsDeviceLost()) device.WaitForGpu();` — **full CPU/GPU serialization every RT frame** |

### The critical structural fact about these two paths

They are **a hard fork, not an integration.** Selected by an `if/else` on a bool at `main.cpp:655`. In RT mode, `BeginFrame`/`DrawSky`/`DrawMesh` are never called, the depth buffer is never cleared, the raster PSOs sit idle. The only shared state is the back buffer itself and two HLSL helper headers (`display_common.hlsli`, `sky_common.hlsli`).

The "display/tonemap unify pass" is **not a pass** — `display_common.hlsli` is 18 lines: clamp to ≥0, multiply by a hardcoded 1.25 exposure, Reinhard `c/(c+1)`, then `saturate` + `pow(1/2.2)`. It's `#include`d inline at `basic_ps.hlsl:145`, `sky_ps.hlsl:18`, and `path_trace.hlsl:514`. Source-level reuse, no HDR intermediate, no post-process stage anywhere.

### The matrix convention (correct, subtle, undocumented at the seam)

`core::Mat4x4` is `m[row][col]` with **row-vector** semantics — translation at `m[3][0..2]`, `TransformPoint` computes `v*M` (`types.h:436-439`). `renderer.cpp:553` computes `wvp = worldMatrix * m_viewProj` (correct order). `UploadCB` memcpys raw row-major bytes. HLSL cbuffers default to **column-major** packing, so the GPU reads the transpose; `basic_vs.hlsl:36` does `mul(worldViewProj, float4(pos,1))` (column-vector form). The two transposes cancel.

It works. Nothing in `renderer.cpp` or `basic_vs.hlsl` states this dependency. A well-meaning `#pragma pack_matrix(row_major)` or a switch to `mul(v, M)` silently breaks every transform in the engine. `types.h:8` actively lies about it ("column-major, D3D convention").

---

## 3. What's genuinely good

Credit where it's earned — several things here are above the level the rest of the code would suggest.

**The math library is correct, and I mean verified, not assumed.** Every LH matrix constructor agrees with DirectXMath byte for byte: `PerspectiveFovLH` (`types.h:398-415`), `LookAtLH` (`types.h:380-394`), `OrthoLH`, `RotationX/Y/Z` (`types.h:329-354`), `RotationQuaternion` (`types.h:356-375`). The depth mapping was hand-verified: z=near → 0, z=far → 1. Both classic traps are avoided:

- The explicit zero-fill loop at `types.h:405-407` clears the `m[3][3]=1` left by the default constructor — the single most common way a projection matrix gets silently broken.
- `InverseTranspose3x3` (`types.h:457-494`) correctly returns cofactor/det, not cofactorᵀ/det. For row-vector storage the normal matrix is `(A⁻¹)ᵀ = C/det`, and the comment at `types.h:486-487` explains exactly why.

Hand-rolled engine math is usually wrong somewhere. This isn't.

**The D3D12 fence and frame-index arithmetic is textbook-correct.** `MoveToNextFrame` (`d3d12_device.cpp:432-444`) signals a monotonic value, records it into the old frame slot, re-reads `GetCurrentBackBufferIndex()`, then waits on the new slot's value — the canonical MS sample pattern, correctly implemented. This is the thing that most commonly goes wrong in a from-scratch bring-up.

**DRED is configured before device creation** (`Init:46 → SetupDRED:536`), which is mandatory and correctly sequenced, and **every GPU object gets `SetName`** (`d3d12_device.cpp:167, 176, 184, 197, 278, 329`) specifically so breadcrumbs are readable. That's a shipping-diagnostics instinct most hobby engines never develop.

**Capability probing is centralized and well done** (`d3d12_device.cpp:555-697`). Shader model probed by descending loop; RT tier gated on `m_device5` being non-null (`617-618`) so `caps.raytracing` can never be true without the interface; tearing probed before swap chain creation so `scDesc.Flags` at line 232 can consume it. Init ordering at `48-56` correctly puts `ProbeCapabilities` before `CreateSwapChain`.

**The root signature is genuinely well-reasoned.** 7 DWORDs: three root CBVs **ordered by update frequency** (b0 per-object/hottest → VS-visible, b1 per-frame → PS-visible, b2 per-material → PS-visible), one descriptor table for t0-t127, one free static sampler. Correct per-stage visibility denies. v1.1 with `DATA_VOLATILE`/`DESCRIPTORS_VOLATILE` plus a complete v1.0 fallback (`renderer.cpp:151-187`). The reasoning is in the comments at `renderer.cpp:52-58`. This is the best-executed code in the render layer.

**The CB ring is correctly synchronized.** 3 UPLOAD buffers, persistently mapped with a zero read range (`renderer.cpp:377-379`), `m_cbOffset` reset each `BeginFrame`, alignment padding zero-filled (`renderer.cpp:465-466`). Because `WaitForCurrentFrame` gates on the fence for that slot before `ResetCommandList`, the buffer being written is guaranteed retired.

**The sparse set itself is textbook-correct.** `component_pool.h:95` uses the robust two-part invariant `denseIdx < m_count && m_dense[denseIdx] == entityIndex` — the back-reference comparison is what makes stale sparse entries harmless, and it's the single most important detail in a hand-rolled sparse set. Swap-and-pop removal (`component_pool.h:71-89`) guards the self-swap case and correctly fixes up `m_sparse[lastEntity]` for the relocated element. The free list is threaded in-place through `Slot::nextFree` with zero extra allocation.

**The DXR plumbing is correct where it's hardest.** SBT alignment (64-byte sections / 32-byte records), root signature ↔ HLSL register agreement, CPU/GPU struct layout matching, AS barriers, initial resource states, per-triangle normals/UVs via global StructuredBuffers, NEE with `ACCEPT_FIRST_HIT | SKIP_CLOSEST_HIT`. This was written by someone reading the spec, not copying a sample.

**Rule 7 (handedness) is visibly enforced.** `renderer.cpp:242-243` sets `CULL_MODE_BACK` with `FrontCounterClockwise = FALSE` and an inline comment "CW = front face (LH)". `mesh.h:141` documents the winding. `debug_overlay.cpp:146` correctly uses `CULL_MODE_NONE` for 2D. This is the best-observed rule in the project — likely because commit `bb49eda "Fix primitive winding and rotation drift"` taught it the hard way.

**Repo hygiene is better than expected.** `git ls-files build` → 0 files. `git ls-files TheDawning_Codex_Handoff_v3_unpacked` → 0 files. 58 tracked files total. `.gitattributes` has explicit per-extension `eol=lf`/`crlf` and binary marks — a real answer to the Windows CRLF problem, not the usual `* text=auto` shrug. Commit granularity is excellent (small, coherent diffstats, no "WIP", no 40-file mega-commits) and docs move with code (`README.md` appears in ~8 of the last 12 commits alongside the feature it documents).

**The comments are honest.** `path_tracer.cpp:596` literally says "this is actually viewProj, not the inverse." Code that admits its own shortcuts is far easier to work on than code that hides them.

---

## 4. Defects worth fixing

All of the below survived adversarial verification. Deduplicated across surveys and ordered by severity.

### HIGH

---

**H1 — GGX NDF denominator epsilon annihilates the specular lobe for smooth materials**
`shaders/path_trace.hlsl:174` and `shaders/basic_ps.hlsl:86`

```hlsl
return a2 / (PI * d * d + 0.0001f);
```

At the lobe peak (`NdotH == 1`), `d` collapses to exactly `a2`, so the true denominator is `PI * a2²`. For roughness 0.25, `a2 = 3.906e-3` and `PI*a2² = 4.79e-5` — **five times smaller than the epsilon being added to it.** The epsilon, not the physics, determines the result:

| roughness | D (code) | D (correct) | ratio |
|---|---|---|---|
| 0.04 | 0.026 | 124339.8 | 0.00002× |
| 0.10 | 1.000 | 3183.1 | 0.0003× |
| 0.25 | 26.40 | 81.49 | 0.324× |
| 0.30 | 26.46 | 39.30 | 0.673× |
| 0.40 | 11.86 | 12.43 | 0.954× |

The whole point of a GGX NDF is the narrow high-energy peak, and this line deletes it for everything below roughness ~0.35. Because **both** paths share the defect it doesn't show as a raster/RT mismatch — it uniformly destroys glossy highlights everywhere. It's masked further in raster by the ad-hoc `ambientSpecular` rim term at `basic_ps.hlsl:139`, which supplies the sheen the real NDF is failing to produce.

**Fix:** `return a2 / max(PI * d * d, 1e-7f);` — multiplicative floor, not additive epsilon. Apply to both files. This is the single largest BRDF error in the suite and the cheapest to fix.

---

**H2 — RNG seed collapses: overflows at frame 16, and is pinned to 0 whenever the camera moves**
`shaders/path_trace.hlsl:137`

```hlsl
return PCGHash(pixel.x + pixel.y * 16384 + frameIndex * 16384 * 16384);
```

`16384 * 16384` is 2²⁸, so `frameIndex * 2²⁸` wraps mod 2³² at frameIndex 16. **Only sixteen distinct frame seeds exist.**

The second half is worse. `g_FrameIndex` is not the wall-clock counter — `path_tracer.cpp:643` uploads `cb.frameIndex = m_accumFrameIndex`, and `path_tracer.cpp:616-619` resets it to 0 on any camera change, where `cameraChanged` triggers on any sub-1e-5 delta in position or basis (`path_tracer.cpp:60-63, 605-609`). **While the camera is moving — i.e. the entire time the user is doing anything — every frame receives the identical seed.** `Random(rngState)` is pure, so the entire random stream is bit-identical frame to frame: same AA jitter (line 306), same hemisphere samples (453), same lobe draws (458), same Russian roulette (494).

**Fix:** use a proper multi-dimensional hash, e.g. `PCGHash(PCGHash(pixel.x) ^ PCGHash(pixel.y) ^ PCGHash(frameIndex))`, and feed it the wall-clock frame counter, not the accumulation index. The accumulation index should reset the *history*, not the *noise*.

---

**H3 — `PathTracer::Resize` ignores `CreateOutputTexture` failure; failure is permanent and leaves dangling UAV descriptors**
`src/render/path_tracer.cpp:359-364`

```cpp
void PathTracer::Resize(ID3D12Device5* device, uint32_t width, uint32_t height)
{
    if (width == m_outputWidth && height == m_outputHeight) return;
    m_outputTexture.Reset();
    m_displayTexture.Reset();
    CreateOutputTexture(device, width, height);   // bool return discarded
    m_accumFrameIndex = 0;
```

Three compounding problems:

1. `CreateOutputTexture` sets `m_outputWidth`/`m_outputHeight` at its **top** (`path_tracer.cpp:296-297`), before any allocation. So after a failure the object reports the new size while owning no textures, and the early-out at line 361 makes every subsequent resize to those dimensions a permanent no-op. **The failure is unrecoverable and silent.**
2. UAV descriptors in heap slots 0 and 1 are only written at lines 344-353, *after* both `CreateCommittedResource` calls. A failure at line 321 or 331 returns early leaving those descriptors pointing at the resources released at lines 362-363. `Dispatch` gates only on `m_initialized` (line 541), which is still true, so `SetComputeRootDescriptorTable(1, ...)` (line 689) binds stale descriptors and `DispatchRays` (line 732) writes through them.
3. `CopyToBackBuffer` bails at line 748 (`if (!m_displayTexture) return;`) *after* `main.cpp:662` already transitioned the back buffer `PRESENT → RENDER_TARGET`. With the overlay off, nothing transitions it back and `ExecuteAndPresent` presents a resource in `RENDER_TARGET` state.

**Fix:** move the `m_outputWidth/Height` assignment to the *end* of `CreateOutputTexture` (success path only); propagate the bool out of `Resize`; add null guards on `m_outputTexture`/`m_displayTexture` in `Dispatch`; and have `CopyToBackBuffer` still restore the back-buffer state on its early-out path.

---

### MEDIUM

---

**M1 — Temporal accumulation does not exist, despite the full CPU-side apparatus driving it**
`shaders/path_trace.hlsl:511`

```hlsl
float3 filteredRadiance = ClampFireflySample(radiance, g_Output[launchIndex].rgb, g_FrameIndex);
float3 accumulatedRadiance = filteredRadiance;   // <-- pass-through alias
```

The history value is read *only* as the clamp reference inside `ClampFireflySample`. It is never blended, never weighted by sample count, never reprojected. The variable name is actively misleading.

All supporting machinery exists and feeds nothing: `path_tracer.cpp:605-623` detects camera motion and maintains `m_accumFrameIndex`; `path_tracer.h:146-156` holds six members purely for this; `path_tracer.cpp:312/323` allocates a dedicated `R16G16B16A16_FLOAT` texture literally named `RT_HDRHistoryTexture`, commented at `path_tracer.h:106` as "HDR history for accumulation." All of it reaches the shader as an RNG seed and a clamp-mode switch.

`CLAUDE.md:30`, `DC_HANDOFF.md`, and `CONVERSATION_CONTEXT.md` all claim "Simple temporal accumulation (10% blend per frame)." **This is the single largest doc-vs-code divergence in the project.** It matters visually: `FullPathTrace` at 4spp/3 bounces with no accumulation shows independent noisy frames that flicker rather than converge.

**Fix:** either implement it (`lerp(history, current, 1.0/(frameIndex+1))` for a proper running mean, invalidated on camera motion) or delete the claim from three documents and rename the variable.

---

**M2 — Indirect specular bounce divides by no PDF and evaluates no BRDF**
`shaders/path_trace.hlsl:472-483`

```hlsl
float3 reflected = reflect(-V, N);
newDir = SampleCosineHemisphere(u, reflected);
newDir = normalize(lerp(reflected, newDir, mat.roughness * mat.roughness));
float3 F0 = lerp(float3(0.04,0.04,0.04), albedo, mat.metallic);
float3 F = FresnelSchlick(saturate(dot(V, normalize(V + newDir))), F0);
throughput *= F;
```

The Monte Carlo weight must be `f(V,L) * dot(N,L) / pdf(L)`. Here `D` is never evaluated, `G` is never evaluated, `dot(N, newDir)` is never applied, and `pdf` is never even **computed**, let alone divided out — the sampling distribution is a cosine lobe about `reflected` non-linearly warped by `normalize(lerp(...))` at line 479, which has no closed form and is certainly not 1.

`throughput *= F` is correct only in the exact `roughness == 0` delta-mirror limit. It's also flatly inconsistent with the Cook-Torrance evaluation used for the *same lobe* under NEE at lines 415-424.

Two consequences:
- **Energy non-conservation.** `F` carries no roughness dependence, so indirect specular reflectance is independent of roughness. A roughness-1.0 metal returns the same ~F fraction per bounce as a perfect mirror, with none of the Smith G masking loss. Rough metals are systematically over-bright.
- **Half the sample budget is wasted on dielectrics.** `specProb = 0.5f + 0.5f * mat.metallic` (line 456) routes 50% of all indirect rays from a pure dielectric into this non-estimator. The diffuse branch is correct (`throughput *= kD * albedo` at 470 exactly equals f·cos/pdf for cosine-sampled Lambertian, then `/= branchPdf` at 487), so diffuse GI is unbiased but converges at half rate while specular GI never converges to anything.

**Fix:** switch to GGX VNDF importance sampling with the analytic PDF and the full `D·G·F·cos/pdf` weight (which simplifies substantially under VNDF).

---

**M3 — Specular bounce has no lower-hemisphere rejection; rays are launched into the surface**
`shaders/path_trace.hlsl:476`

`SampleCosineHemisphere(u, reflected)` generates a full ±90° cosine hemisphere centered on the **mirror direction**, not the shading normal. `reflected` is guaranteed upper-hemisphere after the ClosestHit flip at `path_trace.hlsl:547-548`, but a ±90° lobe about it is not. At grazing view angles — most of any curved object's silhouette — a large fraction of the lobe falls below the tangent plane and `dot(newDir, N) < 0`.

There is no guard. Lines 500-501 unconditionally launch it:

```hlsl
currentOrigin = hitPos + N * 0.001f;
currentDir    = newDir;
```

so the ray starts 1 mm *above* the surface travelling *into* it. The roughness² lerp doesn't save this — at roughness 0.9 the factor is 0.81, so the warped direction stays close to the raw sample. This is also the NaN reachability for `normalize(V + newDir)` at lines 468/482, which is only safe when both arguments share a hemisphere.

**Fix:** reject and terminate (or resample) when `dot(newDir, N) <= 0`. VNDF sampling (M2's fix) largely eliminates this by construction.

---

**M4 — Debug overlay's single upload buffer is written by the CPU while two prior frames may still be reading it**
`src/render/debug_overlay.cpp:437`

`DebugOverlay` owns exactly one persistently-mapped upload buffer and one texture (`debug_overlay.h:68-70`), but `kFrameCount = 3` (`d3d12_device.h:31`). Every frame the CPU rewrites the whole buffer at record time:

```cpp
std::memcpy(m_uploadMapped + (size_t)y * m_uploadPitch,
            m_pixels.data() + (size_t)y * kOverlayWidth * 4,
            kOverlayWidth * 4);
```

then records `cmd->CopyTextureRegion(...)` reading that same memory (line 465).

`MoveToNextFrame` only guarantees frame N−2 has retired. Frames N−1 and N can still be executing their copy when the CPU memcpy for frame N+1 lands. The *texture* is safe (the `PIXEL_SHADER_RESOURCE → COPY_DEST → PIXEL_SHADER_RESOURCE` barriers at `debug_overlay.cpp:443-449/467-469` serialize it on the single direct queue) — only the UPLOAD-heap buffer is unprotected, **because barriers do not synchronize CPU writes to mapped memory.**

`m_pixels` genuinely changes every frame (FPS/frame-ms/camera position), so contents differ. The D3D12 debug layer will **not** report this — it's a data race, not a state error. The path-tracing branch is accidentally immune only because of the `WaitForGpu()` at `main.cpp:755-756`. **Raster is the default boot mode and is fully exposed.**

This is the only defect on this list that fires on the ordinary happy path with no allocation failure required.

**Fix:** triple-buffer the upload buffer (and the texture, or keep the barriers), indexed by `m_frameIndex`.

---

**M5 — `CreateMesh` returns a mesh reporting `IsValid() == true` after upload failure**
`src/render/mesh.cpp:206` (and `CreateMesh32` at `mesh.cpp:272-276`)

`IsValid()` is `vertexBuffer && indexBuffer && indexCount > 0` (`mesh.h:96`). `CreateMesh` sets `indexCount` at line 182 and creates the DEFAULT-heap buffers at 189-192, *then* bails:

```cpp
if (!outVertexUpload || !outIndexUpload)
{
    core::Log::Error("Failed to create mesh staging upload buffers");
    return mesh;      // all three IsValid() predicates already satisfied
}
```

The view setup at 221-227 is skipped, so `vbView.BufferLocation == 0` and `ibView.BufferLocation == 0`, the VRAM is uninitialized, and the buffers are still in `COPY_DEST`.

Second independent hole with the same outcome: `UploadBufferData` returns early on Map failure *before* recording the copy or the transition (`mesh.cpp:67-72`), yet `CreateMesh` continues to 221-227 and builds fully-populated, valid-looking views over buffers that were never written.

Nothing downstream re-checks: `Scene::RenderEntities` gates only on `IsValid()` (`scene.cpp:132`); `Scene::EnsureBLAS` gates only on `IsValid()` (`scene.cpp:197`) before `BuildBLAS`, which reads `mesh.vbView.BufferLocation` at `rt_acceleration.cpp:108`.

**Fix:** on any failure path, `mesh = Mesh{};` before returning. Propagate `UploadBufferData`'s failure to the caller.

---

**M6 — `Registry` discards the generation on every component operation, negating the generational-handle design**
`src/ecs/registry.h:79, 86, 92, 98`

`Has<T>`, `Get<T>`, and `Remove<T>` all route through `entity.Index()` and never call `m_entities.IsAlive(entity)`. The generation is validated in exactly one function — `EntityManager::IsAlive` (`entity.h:94-99`) — which **no component path invokes.**

Failure sequence: destroy `E(index=5, gen=0)` → `Registry::Destroy` correctly strips components and bumps the slot to gen=1 → `Create()` recycles slot 5 returning `E'(5, gen=1)` → `Assign<Transform>(E')`. Now the stale handle `E(5, gen=0)` returns `true` from `Has<Transform>(E)` and `Get<Transform>(E)` hands back **E'`s** Transform. Silent wrong-entity read/write with no diagnostic.

This negates the entire reason the handle is 20+12 bits instead of a raw index.

**Fix:** early-out on `!m_entities.IsAlive(entity)` in all four methods. Currently latent only because `Scene::DestroyEntity` (`scene.cpp:74`) has **zero callers anywhere in the repo** — the removal path has never executed once at runtime.

---

**M7 — Window resize ignores `D3D12Device::Resize` failure and clears the retry flag, leaving null render targets**
`src/main.cpp:629`

```cpp
if (window.WasResized())
{
    device.Resize(window.GetWidth(), window.GetHeight());   // return ignored
    ...
    window.ClearResizeFlag();
}
```

`Resize` releases the back buffers and depth buffer *before* it can know whether the resize will succeed (`d3d12_device.cpp:473-475`), then has four failure exits: line 465 (zero dimensions), 482 (`CHECK_HR` on `ResizeBuffers`), 489 (`CreateRTVs`), 490 (`CreateDepthBuffer`). On any of them every `m_renderTargets[i]` is null and `m_depthBuffer` is null, while `m_width/m_height` retain their old values (only updated at 484-485, after the check).

`CurrentBackBuffer()` (`d3d12_device.h:96`) is `return m_renderTargets[m_frameIndex].Get();` with no null guard. The very next thing the frame does is `TransitionResource(device.CurrentBackBuffer(), ...)` (`main.cpp:662` or `:703`), building a barrier with `pResource = nullptr`, followed by `ClearRenderTargetView` on an RTV whose resource no longer exists. Because `ClearResizeFlag()` runs regardless, **the engine never retries.**

There's also no `m_deviceLost` guard on entry, so a resize during device-removed state attempts `ResizeBuffers` on a dead swap chain.

**Fix:** check the return; on failure, keep the resize flag set, log, and skip rendering that frame. Add a `m_deviceLost` early-out.

---

**M8 — Raster tangent frame drops the sign of the UV determinant, flipping T on every normal-mapped surface in the demo**
`shaders/basic_ps.hlsl:62`

```hlsl
float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
if (abs(det) >= 1e-6)
{
    float3 derivedT = dpdx * duvdy.y - dpdy * duvdx.y;   // numerator only
```

The correct tangent is `(dpdx*duvdy.y - dpdy*duvdx.y) / det`. `det` is used solely as a degeneracy guard and its **sign is discarded**. Line 65 normalizes, so magnitude doesn't matter but sign does: `T_computed = sign(det) * T_true`.

Not hypothetical — **both normal-mapped objects in the demo have det < 0.** `GroundPlane` (`main.cpp:405`) comes from `GeneratePlane` with u ∝ +x, v ∝ +z (`mesh.cpp:378-387`) and triangle order (tl, bl, br) (`mesh.cpp:404-406`), giving `det = -du*dv < 0`. `BlueCube` (`main.cpp:410`) gets `det = -1` on its +Z face from the UVs at `mesh.cpp:312-315` with the (base+0, base+2, base+1) index order at `mesh.cpp:317-319`.

The DXR path does this **correctly** — `path_trace.hlsl:265-267` computes `invDet` and applies it, then corrects B's handedness at 273-274. So the two paths produce genuinely different lighting on the same geometry, and F1 makes it directly comparable.

**Fix:** `float3 derivedT = (dpdx * duvdy.y - dpdy * duvdx.y) / det;`

---

**M9 — The smoke test's only runtime-error assertion can never match**
`tools/smoke_test.ps1:57`

```powershell
$errors = Select-String -LiteralPath $log -Pattern "\[ERROR\]"
```

`core::Log` never emits `[ERROR]`. `src/core/log.cpp:81` uses a four-character column-aligned prefix:

```cpp
if (level == LogLevel::Error) prefix = "[ERR ]";
```

Lines 57-61 are dead code. The other half: `main.cpp` sets a nonzero exit code in exactly two places — `exitCode = 2` when `ensurePathTracing()` fails (`main.cpp:574-576`) and `exitCode = 3` on device loss (`main.cpp:751`). Nothing in the frame loop can fail the run.

This matters concretely: `PathTracer::Dispatch` has **six** `Log::Error(...); return;` paths that skip `DispatchRays` entirely (`path_tracer.cpp:546, 552, 558, 566, 572, 662`) while the frame loop continues. `Scene::CopyPathTraceToBackBuffer` still copies the never-written display texture, `"Smoke mode complete"` is logged, and the process exits 0. **A path tracer that renders literally nothing prints "Smoke test passed (path tracing stable)."**

`README.md:62` claims the harness "fails if the runtime log contains errors." It cannot.

**Fix:** change the pattern to `\[ERR ?\]`, and make `main.cpp` track a "logged an error" flag that forces a nonzero exit.

---

### LOW

---

**L1 — DRED breadcrumb handler dereferences `pLastBreadcrumbValue` without a null check**
`src/render/d3d12_device.cpp:399` and `:404`

```cpp
core::Log::Errorf("  DRED Breadcrumb: CmdList='%s', completed %u/%u ops",
                  name, *node->pLastBreadcrumbValue, node->BreadcrumbCount);
```

That pointer is legitimately null for nodes whose breadcrumb buffer wasn't resident at removal — Microsoft's own samples guard it. The code carefully null-checks `pCommandListDebugNameW` at line 393 but not this. **This crashes inside the post-mortem handler**, destroying the breadcrumb dump, the page-fault VA report at 410-415, and the `m_deviceLost` bookkeeping at 423 — converting a diagnosable GPU crash into an unexplained process crash. Disproportionate cost for a one-line fix.

---

**L2 — Four `Map()` calls discard their HRESULT, then dereference the null pointer**

- `path_tracer.cpp:402` (`CreateConstantBuffer`) → consumed at line 650 `memcpy(m_cbMapped[m_frameIndex], &cb, sizeof(cb));`
- `path_tracer.cpp:438` (`CreateMaterialBuffer`) → consumed at line 655
- `rt_acceleration.cpp:224` (`BuildTLAS`) → worse: `m_tlasMaxInstances = newMax` is set regardless, so every subsequent call skips the grow branch and goes straight to the null write at 230-233. **Permanently.**
- `rt_pipeline.cpp:370` (`BuildShaderTable`) → `memset(mapped, 0, totalSize)` immediately after

The codebase demonstrably knows the correct pattern — `CreateMappedUploadBuffer` in the same file checks properly (`path_tracer.cpp:49-55`), as does `texture.cpp:354-361`. These four are the outliers.

---

**L3 — RT material buffer fixed at 256 entries with a silent clamp while DispatchRays covers all instances**
`src/render/path_tracer.cpp:653`

`CreateMaterialBuffer(dev5, 256)` (`path_tracer.cpp:90`) is a hard cap with no growth path — unlike every other RT geometry buffer, which all get an `Ensure*` helper (`path_tracer.cpp:446-516`). The upload clamps:

```cpp
uint32_t matCount = materialCount < m_maxMaterials ? materialCount : m_maxMaterials;
```

but the validation guard at line 556 only cross-checks the three CPU-side counts against each other; it never compares anything to `m_maxMaterials`. With 300 instances the dispatch proceeds, `SetComputeRootShaderResourceView(3, ...)` (line 695) binds a 256-element buffer as a **root SRV** — no descriptor, no size information, no bounds check — and `path_trace.hlsl` indexes `g_Materials[InstanceID()]` for 256..299. `Scene::BuildAccelerationStructures` assigns `instanceID` monotonically with no cap (`scene.cpp:261`). Genuine OOB read past the allocation.

**Fix:** give it an `EnsureMaterialBuffer` like its siblings.

---

**L4 — `EntityManager::Grow()` reads one Slot past the end of the old array**
`src/ecs/entity.h:69`, `:120`

```cpp
uint32_t index = m_slotCount++;   // m_slotCount is now index+1
if (index >= m_capacity)
    Grow();
```

By the time `Grow()` runs, `m_slotCount == index + 1` but the old array holds only `m_capacity == index` elements. The copy loop `for (i = 0; i < m_slotCount; i++)` reads `m_slots[m_capacity]` — one full 12-byte `Slot` past the allocation. UB; instant ASan/PageHeap hit.

Never fired because `main.cpp:404-435` creates ~12 entities and the first growth threshold is 256 (`entity.h:114`), and no slot is ever recycled.

**Fix:** hoist `Grow()` above the post-increment.

---

**L5 — DXR closest-hit transforms normals by the world matrix instead of its inverse transpose**
`shaders/path_trace.hlsl:539-544`

```hlsl
float3x4 objToWorld = ObjectToWorld3x4();
float3 worldNormal = normalize(float3(
    dot(objToWorld[0].xyz, objectNormal), ...));
```

Normals are covectors. The raster path gets this right (`renderer.cpp:557` → `basic_vs.hlsl:38`). `ecs::Transform::scale` is a full `Vec3f` (`components.h:23`) composed into `Mat4x4::Scaling` with nothing constraining uniformity, so non-uniform scale is fully expressible. Latent only because all five entity definitions in `main.cpp` (lines 406, 411, 417, 422, 433) use uniform scale, under which `normalize()` recovers the correct direction. The error compounds — the skewed normal feeds `ApplyNormalMap` at line 554 as the TBN's N axis.

**Fix:** upload the per-instance normal matrix in the instance metadata buffer alongside everything else already flowing there.

---

**L6 — Raster sky is a screen-space gradient while the RT sky is view-direction dependent**
`shaders/sky_ps.hlsl:16`

```hlsl
float skyBlend = 1.0 - saturate(input.uv.y);
float3 radiance = DawningSkyRadianceFromBlend(skyBlend);
```

The path tracer calls `DawningSkyRadiance(currentDir)` (`path_trace.hlsl:369`), a function of world-space elevation. `sky_common.hlsli` exposes the direction-based entry point **specifically so the two paths can agree**, and the raster path never calls it. The gradient polarity is correct but it's nailed to the framebuffer: it doesn't rotate, doesn't respond to pitch, doesn't respond to FOV. Pitch the camera and press F1 — the horizon jumps.

`Renderer::BeginFrame` already caches `m_viewProj` (`renderer.cpp:495`) and the sky PSO shares the main root signature (`renderer.cpp:297`), so the camera basis is available and simply unused.

---

**L7 — Firefly clamp is history-recursive and hard-clamps to luminance 8.0 on every frame the camera moves**
`shaders/path_trace.hlsl:195-212`

Because `m_accumFrameIndex` resets to 0 on any camera motion (`path_tracer.cpp:616-619`), the `frameIndex == 0` branch is taken **every frame while the user is moving** — highlights hard-clamped to luminance 8.0 continuously, then released the moment the camera stops.

Once stationary, `float maxLum = max(1.5f, previousLum * 3.0f + 0.25f);` (line 210) bounds against the previous frame's already-clamped output. With no accumulation (M1), `g_Output` holds last frame's clamped radiance, so a bright region can only grow 3× per frame from whatever it was previously bounded to. That's a multiplicative ratchet, not a firefly filter — it biases the estimator downward by an amount that depends on frame history rather than sample statistics.

Also an Inf→NaN converter: `sampleRadiance *= maxLum / sampleLum` with `sampleLum == Inf` evaluates `Inf * 0` = NaN, written to `g_Output` at line 513.

---

**L8 — Ad-hoc ambient is added at every path vertex on top of sky radiance the BSDF rays already gather**
`shaders/path_trace.hlsl:441-445`

```hlsl
float3 ambient = albedo * g_AmbientColor.rgb * (1.0f - mat.metallic);
radiance += throughput * ambient * (bounce == 0 ? 0.3f : 0.1f);
```

This is the **full path trace** branch — the mode that's supposed to be physically based. BSDF rays that escape already collect environment energy at line 369. Adding `ambient` at every vertex double-counts it, with magic constants (0.3/0.1, and 2.5/0.25 in the preview branch) corresponding to no physical quantity. It defeats the multi-bounce loop: no number of additional bounces converges to ground truth because the error is added *per bounce* rather than reduced per bounce.

---

**L9 — Feature-level log line always prints "12.0"**
`src/render/d3d12_device.cpp:682`

```cpp
core::Log::Infof("  Feature Level: 12.%d", (m_caps.maxFeatureLevel & 0xF));
```

`D3D_FEATURE_LEVEL_12_0/12_1/12_2` are `0xc000/0xc100/0xc200`. The minor version is in bits 8-11. Needs `(maxFeatureLevel >> 8) & 0xF`. The probe at 592-602 is correct; only the log lies. Note the adjacent shader-model decode at 585-586 gets the equivalent extraction right, which marks this as a copy-paste slip.

---

**L10 — Miscellaneous, cheap**

- `d3d12_device.cpp:365` — `m_cmdList->Close()` discards its HRESULT and line 369 submits the list anyway. This is the single most valuable HRESULT in the file and it's the one ignored; it converts a loggable recording error into an opaque device removal.
- `types.h:193-204` — `Quatf::FromEuler` maps parameters to the **wrong axes**. It's the Wikipedia ZYX aerospace formula verbatim (slots roll=X, pitch=Y, yaw=Z) with parameters named `(pitchRad, yawRad, rollRad)`. Verified: `FromEuler(p,0,0)` yields a rotation about **+Y**. Zero callers today — a trap armed for whoever first writes ship orientation code.
- `types.h:8` — header comment says "column-major, D3D convention." Storage is row-major and nothing ever transposes. Given that correctness depends on an unstated HLSL packing interaction, this comment is actively dangerous.
- `registry.h:92, 98, 105` — `Get<T>` null-derefs when `GetPool<T>()` returns nullptr (no entity has ever been assigned a T). `Has<T>` and `Remove<T>` correctly null-check; `Get` doesn't.
- `registry.h:176-187` — `Each<A,B,C>`: `smallest` assigned and never read, `minCount` computed and never used, loop unconditionally iterates pool A while the comment claims a smallest-pool optimization. Both unused locals trip C4189 under /W4.
- `registry.h:152, 161` — `Each<>` hands the callback references directly into the dense arrays. Assign any A or B from inside the callback and `EnsureDense` (`component_pool.h:146-169`) reallocates, dangling them. No reentrancy guard, no comment. Spawn-during-update is the single most common thing gameplay systems do.
- **16 files use `uint32_t`/`uint64_t` without `#include <cstdint>`** — `main.cpp`, `core/input.cpp`, `core/timer.cpp`, `core/window.cpp`, `ecs/registry.h`, `scene/scene.{h,cpp}`, `scene/resource_manager.cpp`, and all seven `render/*.cpp`. They compile only because `<windows.h>` drags it in transitively. `ecs/components.h` does it correctly, so the rule is understood, just unenforced. CLAUDE.md Rule 4.
- `window.cpp:152-155` — WndProc dereferences `cs->lpCreateParams` with no null check.
- `window.cpp:70` — `RegisterRawInputDevices` return ignored; on failure mouse look dies silently.
- `input.cpp:11` — `static InputState s_prevState;` declared, never read or written.
- `CLAUDE.md` BUILD section says `cd D:\SCIFI_game\TheDawning`. The project is at `D:\The Dawning (new)\The Dawning`. Anyone following it literally fails at step one.

---

## 5. Structural risks

Ordered by how expensive they get if deferred.

**1. `Vec3f` world positions — this is the highest-leverage item in the entire backlog.**
`ecs::Transform::position` is `Vec3f` (`components.h:21`). `Camera::m_position` is `Vec3f` (`camera.h:56`). **`Vec3d` has zero references anywhere outside `types.h`.** CLAUDE.md Rule 1 is unimplemented engine-wide.

Worse, `Vec3d` as written *cannot* receive the conversion — it has no `Cross()`, no `operator*=`, no `operator/=`, no scalar-left `operator*`, no `Lerp` (`types.h:92-126`), all of which `Vec3f` has.

At 10⁷ m from origin, float spacing is ~1 m. With `m_farZ = 10000.0f`, the precision cliff arrives well inside a single star system. Changing this type touches `Transform`, `Camera`, every system, both draw paths, the TLAS instance descriptors, the RT constant buffer, and the shadow-ray epsilons (`path_trace.hlsl:348` uses a fixed 0.001 offset in a codebase targeting planetary scale). **It is currently 12 entities and one draw path. It will never be cheaper than today.**

**2. Duplicated light transport between the two paths.**
They share two HLSL helper headers and nothing else. They have already measurably diverged in four ways: normal transform (L5), tangent handedness (M8), sky evaluation (L6), and roughness clamping (raster clamps to [0.04, 1.0], RT doesn't). The one thing they *do* share is a bug — the identical GGX epsilon (H1). **Shared code stays consistent; duplicated code diverges.** That is exactly what happened, and F1 makes every divergence directly A/B comparable to the user.

Compounding it: `Scene` has two separate extraction loops (`scene.cpp:225-269` and `337-394`) duplicating an identical five-condition filter chain, where the compacted `instanceID` from the first is used as the array index into materials built by the second. Any divergence between those chains silently misassigns every material in the scene.

**3. The frame structure lives in `WinMain`.**
No render graph, no pass abstraction, no HDR intermediate. Both raster PSOs hardcode `R8G8B8A8_UNORM` and tonemap in the pixel shader. Bloom, exposure, TAA, and any post-process require restructuring both PSOs and three shaders. Post-process isn't "later work" — it's blocked.

**4. No resource-lifetime discipline against frames in flight.**
The overlay's single upload buffer races on the default boot path today (M4). Every per-frame RT upload buffer except the constant buffer is single-instanced against 3 frames (`path_tracer.h:125-144`, `rt_acceleration.h:90`), TLAS result and shader table included. The `WaitForGpu()` at `main.cpp:755-756` is masking, not solving — remove the stall and the races become visible; keep it and RT throughput is capped at zero frames in flight forever. There is no `DeferredRelease` and no per-frame ring abstraction, so each new subsystem re-invents the same mistake.

Related and dangerous: `resource_manager.h:11` advertises "Deferred GPU resource release via fence-guarded queue" and line 20 includes `<queue>` for it. **There is no queue, no fence, no deferred path** — `RemoveMesh` drops the ComPtr synchronously. That header comment tells the next engineer that mid-frame release is safe when it's a use-after-free.

**5. Descriptor management blocks the literal next milestone.**
Three shader-visible heaps, monotonic allocation, no free lists. `Renderer::RegisterTexture` allocates from a counter into a fixed 128-slot heap with no way to reclaim, and `Texture` has no destructor. Any runtime texture churn exhausts it. The stated Layer 4 goal is SM 6.6 bindless, which requires **one** global shader-visible heap. The current topology structurally prevents it.

**6. Handle typing.**
`MeshHandle`, `MaterialHandle`, `TextureHandle` are all `using X = ResourceHandle` (`resource_manager.h:47-49`) — zero compile-time protection. `ecs::Material` stores texture references as bare `uint32_t` (`components.h:60-61`), so `main.cpp` passes `hGroundTex.value` positionally into an aggregate initializer — exactly the mistake the type system should catch. Also: `m_meshToBLAS` is keyed by `handle.Index()` alone (`scene.cpp:187, 237, 349`), so recycling a mesh slot silently renders the old mesh's geometry in RT while raster uses the full handle correctly. Same data, two key policies.

**7. The ECS cannot carry a space sim as written.** Beyond the `Vec3f` issue: no hierarchy (`Parent` at `components.h:76` is declared, never used, stores a bare `uint32_t` with no generation, and `Transform::ToMatrix` produces a purely local matrix); no physics consumer (`Velocity` declared and never read; `main.cpp:640` is `while (timer.ConsumeFixedStep()) { /* future physics */ }`); no lifecycle hooks, no dirty tracking, no `Clear()`, no serialization. A space sim is *made of* attached frames — turrets on hulls, ships in bays, everything relative to a moving parent.

**8. Zero test coverage.** No `tests/`, no `enable_testing()`, no `add_test()`, no CI config anywhere. `/W4` is set with `/WX-` and the comment "not yet," so it enforces nothing. `/utf-8` is absent while 37 source files contain non-ASCII bytes with no BOM. The entire automated verification budget is one PowerShell script requiring a DXR GPU, a Debug build, and `dxcompiler.dll`/`dxil.dll` — and its most important assertion is dead code (M9).

**9. Error handling is bimodal.** Init paths check every HRESULT via `CHECK_HR`. The per-frame path checks almost nothing: `Close()` (L10), both `Reset()` calls (`d3d12_device.cpp:358-359`), four `Map()` calls (L2). The knowledge exists in the codebase; it just isn't applied uniformly.

---

## 6. Ambition vs. reality

### The doc corpus splits into three tiers written at different times about different codebases

**Tier A — root-level status docs** (`CLAUDE.md`, `README.md`, `DC_HANDOFF.md`, `CONVERSATION_CONTEXT.md`, ~1,180 lines). Claim Layers 1, 2, 3, 3+RT all **DONE**, with the governing quality bar at `CLAUDE.md:6-7`: *"each layer is complete, tested, and AAA quality before the next begins."*

**Tier B — the consolidation layer** (`UNIFIED_ANALYSIS_RESPONSE.md`, 824 lines; `MASTER_ENGINE_SPEC.md`; `FULL_3D_SPACE_SIM_ROADMAP.md`). **This is the strongest document in the repo and it is already correct about most of what this analysis found independently.** It scores Testing/CI at 1/10, Repository discipline 1/10, Asset pipeline 2/10, Rendering foundation 5/10. It identifies the Vec3f/Vec3d contradiction by name. It prescribes a Phase 0 of "repository, CI, tests" *before* more features. It warns that "a system is not complete because it has a header, a component, a data model, a tick call, and documentation."

**Tier C — the research library** (18 Deep Dive / Bible docs, ~5,900 lines). Genuinely good and genuinely enormous: Kepler propagation with real μ values, GJK/EPA + sequential impulses, EVE-style price discovery, authoritative-server netcode at 30 Hz, chunked `.tdsave` with CRC32, cube-mapped quadtree planetary terrain to depth 15 (~0.3 m). `MASTER_INDEX.md` frames it as a 3,000-step curriculum with a "V2 target ~1M+ lines."

Tier C is a specification for a mid-sized studio's multi-year output. **Tier B knows that. Tier A does not.**

### Layer status

| Layer | Doc claim | Real status | Substance | What's actually missing |
|---|---|---|---|---|
| **1 — Kernel** | DONE, "AAA quality" | **Mostly real** | ~85% | Verified-correct LH math (projection depth mapping and row-vector normal matrix both right, both classic traps avoided). But: `Vec3d` has **zero references outside `types.h`**; no `Mat4x4::Inverse()`; `Quatf::FromEuler` maps parameters to the **wrong axes** (latent, zero callers); no DPI awareness; logger does 4 I/O ops + `fflush` per call with no level filter; **no unit tests for any of it** |
| **2 — Geometry** | DONE | **Real but minimal** | ~70% | Root sig v1.1 w/ v1.0 fallback, frequency-ordered slots, 256-byte-aligned CB ring — all genuinely well done. But no frustum culling, no draw sorting, no batching or instancing, no MSAA, no alpha blending, no HDR intermediate, no post-process stage, no fullscreen. `renderer.h:7`'s own TODO ("stream upgrade in Layer 3") is unresolved with Layer 3 marked DONE |
| **3 — ECS** | DONE | **Correct core, unhardened layer** | ~60% | The sparse set itself is textbook-correct. But `Registry` **discards the generation on every component op**, negating the entire generational-handle design; `Get<T>` null-derefs on an unseen type; the 3-component `Each` has dead "smallest pool" variables and a comment describing an optimization the code doesn't have. Decisively: `Scene::DestroyEntity` is **never called from anywhere** — the removal path, free-list recycle, generation bump, and swap-back have never executed once at runtime |
| **3+RT — Path Tracing** | DONE | **Prototype wearing production docs** | ~50% | SBT alignment, root-sig/HLSL register agreement, AS barriers, per-triangle normal/UV metadata — all correct and non-trivial. But: **temporal accumulation does not exist**; indirect specular has no BRDF eval and **no PDF division**, so it is not an estimator of anything; every per-frame upload buffer except the CB is **single-buffered against 3 frames in flight**; camera FOV is **hardcoded `70.0f` in the shader**; the shipped default mode is 1 bounce with ad-hoc ambient fudge constants |
| **4 — Materials** | `CLAUDE.md`: not started. `README.md`: "Layer 4 material path" | **Partially real; the two docs contradict each other** | ~40% | Real: KTX/PNG/DDS loading, CPU mip gen, normal maps, GGX raster shading, indexed texture tables in both paths. Missing: SM 6.6 bindless (raster is still SM 5.1 via FXC), HDR render target, tone-map pass, exposure, metallic/roughness/AO/emissive maps, shadow maps, glTF or *any* real mesh loading. `assets/textures/` contains **only a README** — a clean clone always falls through to procedural checkers, so the KTX and WIC loaders are unexercised code |
| **5 — World Foundation** | Planned | **0%, and actively contradicted** | 0% | `Transform::position` is `Vec3f`, `Camera::m_position` is `Vec3f`, `Vec3d` is dead code — and `Vec3d` as written *cannot* support camera-relative math (no `Cross`, no `operator*=`, no scalar-left multiply, no `Lerp`). No terrain, no atmosphere, no sky dome, no star field. **`CLAUDE.md` Rule 1 is violated engine-wide** |
| **Physics / fixed step** | Rule 6, "accumulator pattern, 60Hz" | **Scaffolded, empty** | ~5% | `main.cpp:640`: `while (timer.ConsumeFixedStep()) { /* future physics */ }`. The accumulator is implemented and correctly fed. Nothing consumes it. `ecs::Velocity` is declared and never referenced |
| **Pillars 4–13** (flight, combat, economy, AI, net, save, UI, animation, terrain, audio, tooling) | ~5,900 lines of Deep Dives | **0%** | 0% | No directories, no files, no stubs. The debug overlay is a fixed 6-line status readout, not a UI system |
| **Testing / CI** | "complete, tested" | **0%** | 0% | No `tests/`, no `enable_testing()`, no `add_test()`, no CI config. `tools/` holds one PowerShell script whose most important assertion **is dead code** |
| **Repo discipline** | Phase 0 exit criterion | **Partially met** | ~50% | Genuinely good: git is clean, 58 tracked files, `build/` and the handoff tree correctly ignored, `.gitattributes` present. Not met: no CI, Release is never built or tested, `dxcompiler.dll`/`dxil.dll` acquisition is fully manual, shader edits may not reach the output dir (POST_BUILD copy with no `DEPENDS`) |

### The claims that are false

- **"Simple temporal accumulation (10% blend per frame)"** — `CLAUDE.md:30`, echoed twice more. Does not exist (M1).
- **"Each layer is complete, tested, and AAA quality"** — `CLAUDE.md:6-7`. Zero tests. Not "few" — zero.
- **"Rule 1: Double precision (Vec3d) for world positions"** — `CLAUDE.md:74`. Unimplemented everywhere. Note `CONVERSATION_CONTEXT.md:72` is *honest* about this ("aspirational for Layer 5+") while `CLAUDE.md` — the file an agent auto-reads — states it as an absolute rule. The two disagree and the wrong one is read first.
- **"Fails if the runtime log contains errors"** — `README.md:62`. Cannot fire (M9).
- **"Raster mode draws a tone-mapped sky gradient from the same sky helper used by DXR"** — `README.md:92`. Same *file*, different *function* (L6).
- **Layer 4 status** — `README.md:13` says "Layer 4 material path"; `CLAUDE.md:53` lists Layer 4 as future work.

### The docs are also stale in the *favorable* direction

`CONVERSATION_CONTEXT.md:464` and `DC_HANDOFF.md:204` both list "normals in closest-hit are approximate" as a known limitation — that was **fixed**; real per-triangle normals and UVs are now supplied via global StructuredBuffers. Same for `CONVERSATION_CONTEXT.md:482` "Logger has no file output," which now has it. **Being unreliable in both directions is worse than being uniformly optimistic** — the docs can't be trusted as either a spec or a TODO list.

### The archive pile

`TheDawning_Codex_Handoff_v3_unpacked/` is **84 MB, of which 69 MB is ~40 zip archives.** Correctly gitignored — verified zero tracked files under it. Credit for that; it's the most important thing to get right.

Still harmful in the working tree:
- `UNIFIED_ANALYSIS_RESPONSE.md` already documented eight **byte-for-byte identical** zip pairs and identified `UIOverhaul`/`SystemsOverhaul`/`Combined` as patch sets, not snapshots. `TheDawning_FinalBeta.zip` contains ~100,831 JSON files. You're paying 69 MB to preserve redundancy that has been formally analyzed and declared redundant.
- `canonical_v3/dawning_v3/` is a **stale live-source duplicate** — 34 files / 6,669 lines against the live tree's 46 / 10,078. Every `grep` returns hits from both trees with nothing in the path to signal which is dead.
- **`CLAUDE(1).md` is an outright trap.** It's the **V2** `CLAUDE.md` describing a `src/` with `physics/`, `ship/`, `gameplay/ # 71 systems`, `audio/`, `ui/`, `npc/`, `world/`, `net/`, `nav/`, `procgen/`. None exists in V3. It also carries a *different* terminology rename map than the root `CLAUDE.md` (Klingon→Xenoes vs Klingon→Vesk).

The **18 Deep Dive / Bible markdown files (~5,900 lines) are the most valuable non-code asset in the project** and deserve rescuing from the pile.

### Separately: `.git` is 42 MB for a repo whose largest blob is a 37 KB `texture.cpp`

`git count-objects -vH` reports `size: 40.81 MiB`, `in-pack: 0`, `packs: 0` — **the repository has never been packed or GC'd even once.** The multi-MB loose objects all decode to ZIP archives (`PK\x03\x04`, first entry `AudioBackend.cpp`) — handoff bundles that were `git add`-ed at some point but never reached a reachable commit. `git fsck --unreachable` confirms all unreachable. `git gc --prune=now` reclaims ~40 MB, >95% of `.git`, with zero risk to history.

### Verdict

**A competent learning project with real engine bones — not a serious engine baseline, and considerably more than a scaffold.**

The case for "more than scaffold" is strong: the math is verifiably correct, the D3D12 sync is correct, the DXR bring-up is correct in the places that are hardest, and a functioning path tracer exists at all. Someone learned a lot of real graphics engineering building this.

The case against "serious baseline" is the gap between claim and code, and it is wide. **It's not that features are missing** — missing features are fine at this stage and mostly honestly labeled in the incomplete notes. **It's that the documentation describes an engine roughly one layer ahead of the code**, and several of the divergences (temporal accumulation, deferred release, "tested") are exactly the ones a reader would rely on without checking.

One more observation, offered directly: the historical baseline is sobering. V1 reached ~60,000 lines and still lacked a proper camera, shadows, sky, post-processing, working animation, real audio output, and terrain LOD, per the project's own gap report. V3 is at ~11,000 lines. `MASTER_INDEX.md`'s "~1M+ lines" is not a plan; it's a number.

`UNIFIED_ANALYSIS_RESPONSE.md` already wrote the correct prescription — *"Do not add another major gameplay domain until that slice works from startup through combat, death, save/load, and clean shutdown."* **The project's failure mode is not lack of ideas or lack of technical skill. It is that the discipline the consolidation doc prescribes has already been partly ignored:** a DXR path tracer was built before Phase 0's tests and CI existed, and it shipped with a headline feature that was never implemented and a smoke test that structurally cannot catch it. Those two facts are the same fact.

---

## 7. Recommended next moves

Opinionated ordering. **Do not start Layer 4.**

### Sprint 0 — under an hour, do it today

1. **`git gc --prune=now`.** Reclaims ~40 MB of unreachable ZIP blobs from a repo that has never been packed. Zero risk, zero history change. Highest effort-to-benefit ratio on this entire list.
2. **Configure a remote and push.** Evidence shows a single local `master`, no remote. 33 hours of work has exactly one copy on one disk.
3. **Move `TheDawning_Codex_Handoff_v3_unpacked/` out of the working tree** to `D:\The Dawning (new)\_archive\`. Before you do: extract the ~25 `.md`/`.docx` Deep Dives into a committed `docs/research/` (~300 KB, versioned, greppable). Then **delete `canonical_v3/` and `CLAUDE(1).md` outright** — they generate false positives with no offsetting value. This turns 84 MB of liability into the reference library it was meant to be, at ~0.4% of the footprint.
4. **Fix the stale build path in `CLAUDE.md`** (`D:\SCIFI_game\TheDawning` → the real path). An onboarding doc whose first command fails undermines trust in everything below it.
5. **Fix the smoke test regex** (`\[ERROR\]` → `\[ERR ?\]`) and add an error-latch to `main.cpp` so any logged error forces a nonzero exit. Until this lands, **you have no automated verification at all** — you have a script that always passes.

### Sprint 1 — make failures detectable (a day)

6. **Add a unit test target.** `enable_testing()` + doctest or Catch2 (single-header, no vcpkg friction) + `tests/`. Start with what has already produced shipped bugs:
   - `PerspectiveFovLH` depth endpoints (z=near → 0, z=far → 1) and `m[3][3] == 0`
   - `LookAt` basis orthonormality and +Z-toward-target
   - `Quatf::FromEuler` axis mapping — **this test fails today** (L10), which is the point
   - `Transform::ToMatrix` composition order
   - Sparse-set add/remove/recycle assertions — **this catches L4 (Grow overread) and M6 (missing generation checks) immediately**
   - CPU/GPU struct layout `static_assert`s for every cbuffer and StructuredBuffer element

   This runs in milliseconds, needs no GPU, and is where the cheapest bugs live. A hundred lines here would have caught two confirmed defects in this report.

7. **Fix H1 (GGX epsilon).** One line in each of two files. It is currently destroying every glossy highlight in both render paths and it takes 60 seconds.
8. **Fix H2 (RNG collapse) and M4 (overlay race).** H2 is a hash change; M4 is triple-buffering one buffer. Both are small and both fire on the default path.
9. **Fix H3, M5, M7, L1, L2** — the failure-handling cluster. All are "check the return value you already have" and together they eliminate every confirmed null-deref and every silent-unrecoverable-state path in the codebase.
10. **Add the missing `<cstdint>` to the 16 files.** Mechanical, ~15 minutes, closes a whole class of future "it compiled yesterday" breaks. Then add a pre-commit hook that greps for it — `.git/hooks` is currently all stock samples, which is what makes Rules 4 and 5 aspirational rather than real.

### Sprint 2 — reconcile documentation with code (half a day)

11. **Go through `CLAUDE.md` and `README.md` and delete every claim that isn't true today.** Specifically: temporal accumulation, "complete, tested, AAA quality," Rule 1 (relabel as "planned — not yet enforced"), the smoke test's error-detection claim, the shared sky helper claim, and the Layer 4 contradiction. Adopt `CONVERSATION_CONTEXT.md:72`'s honest framing throughout.
12. **Fix the `resource_manager.h:11` comment.** It documents a fence-guarded deferred-release system that does not exist. That line tells the next engineer mid-frame release is safe when it's a use-after-free — it is the single most dangerous line in the codebase.
13. **Either implement temporal accumulation or delete it.** If implementing: a running mean (`lerp(history, current, 1.0/(n+1))`) invalidated on camera motion, ~10 lines, and the entire CPU apparatus already exists to drive it. If deleting: rename `accumulatedRadiance`, remove the history texture, and pull the claim from three docs.

### Sprint 3 — the change that only gets more expensive (a week)

14. **Do the `Vec3d` / camera-relative retrofit now.** At 12 entities and one draw path this is a week. At 40k lines it's a month and a source of bugs for a year. Order of operations:
    a. Fill out `Vec3d` so it can actually be used — `Cross`, compound assignment, scalar-left multiply, `Lerp`. Add `Quatd` if world orientation needs it.
    b. Add `Mat4x4::Inverse()`. This also kills the hardcoded `float fovRad = 70.0f` at `path_trace.hlsl:315` and the `path_tracer.cpp:596-597` workaround.
    c. Change `ecs::Transform::position` and `Camera::m_position` to `Vec3d`.
    d. Subtract camera position at the extraction boundary — one place in `Scene`, feeding both paths.
    e. Write the tests first: a position at 10⁷ m should still render stably.

    This is also the natural moment to fix the shadow-ray epsilon (`path_trace.hlsl:348`) to be distance-scaled.

### Sprint 4 — unblock everything downstream

15. **Extract an `App` class from `WinMain`** and move the barriers and frame structure out of `main.cpp` into a pass/frame abstraction. Nothing else in the render layer can improve until this happens.
16. **Add a per-frame resource ring abstraction and a `DeferredRelease` queue to `D3D12Device`,** plus a real descriptor allocator with a single global shader-visible heap. This eliminates the seven hand-rolled upload sites, kills the RT single-buffering risks, lets you delete the `WaitForGpu()` stall at `main.cpp:755-756`, and is a hard prerequisite for SM 6.6 bindless.
17. **Move the BRDF and material evaluation into a shared `.hlsli`** consumed by both `basic_ps.hlsl` and `path_trace.hlsl`, and unify scene extraction into one CPU-side pass feeding both. This is what stops M8/L5/L6-class divergences from recurring every time you touch either path.
18. **Give the smoke test eyes.** Add `--smoke-capture` doing a back-buffer readback on the final frame, then assert cheap robust invariants in PowerShell: not uniformly black, not uniformly one color, no NaN/Inf, mean luminance in a sane band. Later upgrade to a reference-image compare. This converts a liveness test into an actual render test and closes the blind spot around orientation, culling, and shader-output bugs — the exact class that produced two of your 28 commits.
19. **Decouple smoke assertions from log prose.** Replace `-match "Sky PSO created"` with structured markers (`[SMOKE] sky_pso=ok`) so human log text stays free to change. Parameterize the hardcoded `build\Debug\` paths with a `-Config` argument — Release is currently never tested at all.
20. **Add a GPU-less CI job.** A GitHub Actions Windows runner can't run DXR, but it can build and run the Sprint-1 unit tests on every push. Turn on `/WX` once it's green.

### Only then

Layer 4. And when you get there, the first thing to write is the HDR render target and tone-map pass, not more material features — because both raster PSOs currently hardcode `R8G8B8A8_UNORM` and tonemap in the pixel shader, and everything downstream of that (bloom, exposure, TAA) is blocked behind it.

---

**Closing note.** Steps 1–13 add zero features. They are entirely what "Layer N is DONE" was supposed to mean. The D3D12 and DXR work here is legitimately good — good enough that the gap between it and the verification around it is the main thing holding the project back. Do steps 1–14 and this becomes a serious baseline. Add Layer 4 on top of it as-is and the divergences compound.
