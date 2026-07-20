#pragma once
// =============================================================================
// render/renderer.h — The Dawning V3 Renderer
// =============================================================================
// Manages the rendering pipeline for Layer 2:
//   - Root signature (v1.1 with fallback to v1.0)
//   - Graphics PSO (classic API — stream upgrade in Layer 3)
//   - Per-frame constant buffer upload ring (persistently mapped, 256-byte aligned)
//   - Draw dispatch for meshes with per-object transforms and materials
//
// Shader binding layout (matches the raster shaders):
//   t0/space2: StructuredBuffer<ObjectData>   — per-draw transforms  (root SRV)
//   t0/space3: StructuredBuffer<MaterialData> — per-draw material    (root SRV)
//   b1:        CBPerFrame   — light, ambient, camera basis, lightViewProj
//   b3:        CBDrawIndex  — { objectIndex, materialIndex, drawProbeEnabled },
//                             root 32-bit constants
//   u0/space4: RWByteAddressBuffer — merged draw-record probe    (root UAV)
//   b4:        CBPerPass    — viewProj; light matrix in the shadow pass, camera
//                             matrix in the main pass. Once per pass, not per draw.
//   b0 and b2 are permanently free: they were CBPerObject and CBMaterial, and
//   the new registers are deliberately NOT reused so a diff cannot confuse
//   "b0 is per-object data" with "b0 is a draw index".
// =============================================================================

#include "d3d12_device.h"
#include "gpu_draw_records.h"   // ObjectData / MaterialData / CBPerPass layouts
#include "descriptor_allocator.h"
#include "environment_ibl.h"
#include "mesh.h"
#include "camera.h"
#include "texture.h"
#include "../core/types.h"
#include "../core/shadow_cascades.h"
#include <cstddef>
#include <cstdint>

namespace render
{

// The merged draw-record probe's reduction. See DrawProbeRecord in
// gpu_draw_records.h for what the GPU writes and why.
//
// SPLIT BY PASS, and that is not cosmetic. A single "mismatch" count tells you
// the GPU disagreed with the CPU and nothing else; you then go looking through
// three shaders. Shadow and main are disjoint ranges of the same buffer, so
// attributing each mismatch to its range costs one comparison and localises a
// failure to a specific vertex shader. The distinct counts do the same job from
// the other direction: the classic breakage - a hardcoded element, a lost root
// constant, SV_InstanceID at SM 5.1 - collapses a whole pass to ONE distinct
// marker, which is a far more legible symptom than N hash mismatches.
struct DrawProbeValidation
{
    // Shadow pass: object buffer range [0, shadowRecords).
    uint32_t shadowRecordsChecked = 0;
    uint32_t shadowDistinctMarkers = 0;
    uint32_t shadowMismatches = 0;

    // Main pass: object buffer range [shadowRecords, objectRecords).
    uint32_t mainRecordsChecked = 0;
    uint32_t mainDistinctMarkers = 0;
    uint32_t mainMismatches = 0;

    // Material records, witnessed by basic_ps at the slot it shaded with.
    //
    // `materialRecordsChecked` counts main-pass slots whose material words were
    // WRITTEN. It can legitimately be less than mainRecordsChecked: the pixel
    // stage does not run for a draw that shades no pixels, so an occluded or
    // fully back-facing draw leaves its material words zero. Those are skipped
    // rather than counted as mismatches, which is why the harness asserts on
    // this count and on materialDistinctMarkers rather than on equality with
    // the main-pass record count.
    uint32_t materialRecordsChecked = 0;
    uint32_t materialDistinctMarkers = 0;
    uint32_t materialMismatches = 0;

    // Main-pass slots the pixel stage never wrote. Reported so that a run where
    // this grows unexpectedly is visible rather than silently shrinking the
    // material evidence.
    uint32_t materialRecordsUnshaded = 0;

    uint32_t ObjectRecordsChecked() const { return shadowRecordsChecked + mainRecordsChecked; }
    uint32_t ObjectMismatches() const { return shadowMismatches + mainMismatches; }
};

// =============================================================================
// Constant buffer structs (must match HLSL cbuffer layouts exactly)
// =============================================================================
struct CBPerFrame
{
    float lightDir[3];
    float pad0;
    float lightColor[3];
    float pad1;
    float ambientColor[3];
    float pad2;
    float eyePos[3];
    float pad3;

    // Camera basis + projection extents, so the raster sky can reconstruct a
    // world-space view direction per pixel and evaluate the same elevation-based
    // sky function DXR uses. Without these the raster sky was a screen-space
    // gradient nailed to the framebuffer: it did not rotate, respond to pitch, or
    // respond to FOV, so the horizon jumped when toggling F1.
    float camRight[3];
    float tanHalfFovY;
    float camUp[3];
    float aspect;
    float camForward[3];
    float pad4;
    // ---- FROZEN PREFIX ENDS HERE, at byte 112 ------------------------------
    // sky_ps.hlsl declares EXACTLY bytes 0..111 of this struct and reads them by
    // offset. Nothing may be inserted above this line: doing so shifts every
    // offset sky_ps.hlsl reads, silently, with no compile error on either side.
    // Fields may only ever be APPENDED below.

    // 112..367 - one camera-relative light view-projection per cascade.
    //
    // Camera-relative because every world matrix the raster path sees already is
    // (RULE 1 in CLAUDE.md); a light matrix in absolute world space would be
    // correct only while the camera sat at the origin.
    //
    // Cascade 0 occupies exactly the bytes the old single lightViewProj did,
    // which is precisely the property that lets sky_ps.hlsl stay untouched.
    float lightViewProj[core::kShadowCascadeCount][16];
    // 368..383 - outer radius of each cascade, camera-relative world units.
    float cascadeSplitRadius[core::kShadowCascadeCount];
    // 384..399 - world units per shadow texel, per cascade. Replaces a hardcoded
    // 24.0f/2048.0f pair that used to live in basic_ps.hlsl with nothing
    // enforcing that it matched the C++ side.
    float cascadeTexelWorld[core::kShadowCascadeCount];
    // 400..415 - inner edge of each cascade's outer blend band. RESERVED: it is
    // uploaded every frame but nothing consumes it yet. It exists from the start
    // so enabling the optional cross-cascade blend later cannot churn the byte
    // layout, which is the one thing sky_ps.hlsl cannot tolerate.
    float cascadeFadeLo[core::kShadowCascadeCount];
};
// These five pin the C++ half of the layout. The HLSL half is pinned
// independently by packoffset on every member of cbuffer CBPerFrame in
// basic_ps.hlsl - see the comment there for why that matters more than it looks.
static_assert(sizeof(CBPerFrame) == 416,
              "CBPerFrame must match cbuffer CBPerFrame (b1) in the raster shaders");
static_assert(offsetof(CBPerFrame, lightViewProj) == 112,
              "sky_ps.hlsl declares bytes 0..111 as a frozen prefix; "
              "lightViewProj must stay at offset 112");
static_assert(offsetof(CBPerFrame, cascadeSplitRadius) == 368, "");
static_assert(offsetof(CBPerFrame, cascadeTexelWorld) == 384, "");
static_assert(offsetof(CBPerFrame, cascadeFadeLo) == 400, "");
static_assert(core::kShadowCascadeCount == 4,
              "the split/texel/fade tables are declared float4 in basic_ps.hlsl; "
              "changing the count means repacking them as float4[(N+3)/4] and "
              "indexing [i/4][i%4]");

// Align size to 256 bytes for CBV placement
constexpr uint32_t AlignCBSize(uint32_t size)
{
    return (size + 255u) & ~255u;
}

// =============================================================================
// Renderer
// =============================================================================
class Renderer
{
public:
    bool Init(D3D12Device& device);
    void Shutdown();

    // Bakes the prefiltered environment cubemap if the sky revision has moved,
    // publishes its SRV at kEnvCubeDescriptorIndex, and runs the Stage 1 GPU
    // assertions. Opens and flushes its own command list, so it must be called
    // OUTSIDE any ResetCommandList/ExecuteCommandLists pair - the App calls it
    // beside InitializeScene, in the same style.
    //
    // Called from Init today. When the sky becomes dynamic this moves into
    // BeginFrameResources, where the revision compare makes the static case one
    // integer test per frame. Returns false when a Stage 1 assertion failed.
    bool EnsureEnvironmentIBL(D3D12Device& device);

    // Call once per frame BEFORE any pass records anything - above the shadow
    // pass, and above the raster/path-tracing branch so an F1 toggle also lands
    // on a clean slot.
    //
    // This exists because BeginFrame is NOT first in a frame. App::RenderFrame
    // runs the entire shadow pass before it, so while BeginFrame was the only
    // place m_currentFrame and m_cbOffset were assigned, every DrawMeshShadow
    // in frame N uploaded into the buffer belonging to frame N-1's slot, at
    // offsets continuing past that frame's high-water mark, with root CBVs
    // pointing into it - and BeginFrame then rewound a DIFFERENT buffer to zero.
    // On frame 0 both indices are zero before and after, so the main pass
    // overwrote the shadow constants at the same addresses before the GPU
    // executed anything. In steady state frame N+3 rewinds a slot that frames
    // N+1 and N+2 may still be reading, since WaitForCurrentFrame only
    // guarantees frame N has retired. It survived only because a constant
    // entity count made the two watermarks abut.
    //
    // maxDrawsHint sizes the per-draw structured buffers. It is an UPPER bound
    // (Scene's MeshInstance pool count, including invisible entities), which is
    // exactly what you want for sizing. Growth happens only here - before any
    // pass records anything and before either root SRV is bound - so an
    // already-bound SRV's virtual address can never be invalidated under a
    // recorded draw.
    void BeginFrameResources(D3D12Device& device, uint32_t maxDrawsHint,
                             bool enableDrawProbe = false);

    // Clears the stale-state guard. Called at the end of App::RenderFrame.
    void EndFrameResources() { m_frameResourcesBegun = false; }

    // Call once per frame before any draw calls
    void BeginFrame(D3D12Device& device, const Camera& camera);

    // Draw the raster sky background before scene geometry.
    void DrawSky(D3D12Device& device);

    // Draw a mesh with a world transform and material properties
    void DrawMesh(D3D12Device& device, const Mesh& mesh,
                  const core::Mat4x4& worldMatrix,
                  const core::Color& albedo = core::Color::White(),
                  float roughness = 0.5f,
                  float metallic = 0.0f,
                  const Texture* albedoTexture = nullptr,
                  const Texture* normalTexture = nullptr,
                  const Texture* ormTexture = nullptr,
                  const Texture* emissiveTexture = nullptr,
                  const core::Color& emissive = core::Color{ 0.0f, 0.0f, 0.0f, 1.0f },
                  float emissiveStrength = 0.0f);

    // Register a texture SRV for raster material sampling.
    DescriptorHandle RegisterTexture(ID3D12Device* device, const Texture& texture);

    // Return a descriptor slot handed out by RegisterTexture. The slot is parked
    // against the current frame's fence and only becomes reusable once the GPU
    // has retired every command list that could reference it - see
    // render/descriptor_allocator.h. Without this every removed texture consumed
    // one of 127 usable slots permanently.
    void ReleaseTextureDescriptor(D3D12Device& device, DescriptorHandle descriptor);
    void ReclaimTextureDescriptors(D3D12Device& device);

    // Constant-ring pressure, for the smoke harness. The only ring
    // instrumentation before this was the overflow error in UploadCB, which
    // fires at 100% - after the offending draws have already bound GPU address
    // zero. A high-water mark gives the harness something to gate on while
    // there is still headroom.
    uint32_t ConstantRingPeakBytes() const { return m_cbPeak; }
    uint32_t ConstantRingCapacity() const { return kCBRingSize; }

    // Per-draw structured-buffer occupancy, for the smoke harness.
    // ShadowRecords and MainRecords are the two passes' disjoint slices of the
    // object buffer, latched at the end of the last completed frame. They must
    // be equal: Scene::RenderShadowCasters and Scene::RenderEntities walk the
    // same pool with identical visible/Transform/Material filters, which is
    // what makes 2 x maxDrawsHint sufficient object capacity - and that parity
    // is enforced nowhere else.
    uint32_t ObjectRecordsPeak() const { return m_objectPeak; }
    uint32_t MaterialRecordsPeak() const { return m_materialPeak; }
    uint32_t ObjectBufferCapacity() const { return m_objectBuffer.capacity; }
    uint32_t MaterialBufferCapacity() const { return m_materialBuffer.capacity; }
    // Times a per-draw structured buffer was REPLACED by a larger one - the one
    // operation here that can use-after-free, because kFrameCount frames may
    // still be reading the buffers it releases and CPU writes to persistently
    // mapped UPLOAD memory are not synchronised by resource barriers.
    //
    // This used to read zero in an ordinary run, which is what --smoke-force-grow
    // existed to work around. It no longer does, and TWO independent mechanisms
    // now keep it off zero, kept from both sides of this merge because they
    // stress different things:
    //
    //   * The capacity floors are smaller than any real scene and Init allocates
    //     AT them, so frame ZERO reallocates. This is the harmless case - see
    //     the in-flight counter below for why it is called that.
    //   * Smoke then RAMPS the draw hint every frame - in BOTH modes; it was
    //     briefly raster-only, which quietly left the DXR run with a quarter of
    //     the coverage - and growth is geometric, so the buffers are replaced
    //     repeatedly mid-run with frames genuinely outstanding. The +80 entities
    //     App::ApplySmokeGrowthStress adds at frame 8 do the same in both modes.
    //
    // -ForceGrow used to be the ONLY way to reach this branch. It no longer is:
    // the sizing-hint ramp runs by default in BOTH smoke modes, so the default
    // run is already heavier than -ForceGrow ever was. The switch is still
    // there and still steepens the ramp - it is the opt-in heavy case, not the
    // coverage floor, and tools/smoke_test.ps1 asserts against the default.
    uint32_t StructuredBufferReallocations() const { return m_structuredBufferReallocations; }
    // The subset of the above that ran with at least one frame already recorded
    // and never waited upon - the genuinely hazardous case, and the only one a
    // missing deferred-release fence can be caught by. Frame zero's grow is NOT
    // in this count: no command list has bound the buffer it releases, so it
    // stays green even with the fence guard deleted outright (measured, not
    // assumed). Deliberately derived from a frame count rather than from any
    // fence value - see EnsureFrameStructuredBuffer for the two fence-based
    // versions that were tried first and why each was wrong.
    uint32_t StructuredBufferReallocationsInFlight() const
    {
        return m_structuredBufferReallocationsInFlight;
    }
    uint32_t ShadowRecords() const { return m_reportedShadowRecords; }
    uint32_t MainRecords() const { return m_reportedMainRecords; }

    // Smoke-only GPU evidence for the root-SRV record contract. The final
    // raster frame hashes the object and material fields the vertex shaders
    // actually read, copies those hashes to READBACK memory, and compares them
    // with the CPU upload records after the queue retires.
    bool RecordDrawProbeReadback(D3D12Device& device);
    bool ReadDrawProbe(DrawProbeValidation& validation);

    // Diagnostics for the smoke harness and for anyone debugging heap pressure.
    uint32_t TextureDescriptorsInUse() const { return m_textureAllocator.InUse(); }
    uint32_t TextureDescriptorHighWater() const { return m_textureAllocator.HighWater(); }
    size_t TextureDescriptorsPending() const { return m_textureAllocator.PendingCount(); }

    // -------------------------------------------------------------------------
    // HDR scene target and tone-map resolve
    // -------------------------------------------------------------------------
    // Raster geometry renders into a linear R16G16B16A16_FLOAT target rather than
    // straight into the 8-bit back buffer, and a fullscreen pass tone-maps it at
    // the end of the frame. Previously each pixel shader tone-mapped its own
    // output, which meant the frame was never available in linear HDR anywhere -
    // bloom, exposure and TAA all need that intermediate, so they were blocked.
    //
    // BeginScenePass  — HDR target to RENDER_TARGET, cleared, bound with depth.
    // ResolveToBackBuffer — HDR to PIXEL_SHADER_RESOURCE, back buffer to
    //                       RENDER_TARGET and bound, fullscreen tone-map drawn.
    //                       Leaves the back buffer in RENDER_TARGET so the
    //                       overlay can draw over it.
    void BeginScenePass(D3D12Device& device);

    // Bloom + tone-map resolve. Bloom runs at half resolution between the scene
    // pass and the resolve, and is composited in LINEAR space before the curve -
    // it is scattered light, so it adds to radiance. Compositing after tone
    // mapping would brighten pixels that are already display-saturated.
    void ResolveToBackBuffer(D3D12Device& device);

    // Post-process tuning. Exposure was a constant baked into
    // display_common.hlsli; it is now a parameter so auto-exposure has somewhere
    // to attach. Setting bloomIntensity to 0 skips the bloom passes entirely.
    void SetExposure(float exposure) { m_exposure = exposure; }
    void SetBloom(float intensity, float threshold, float softKnee)
    {
        m_bloomIntensity = intensity;
        m_bloomThreshold = threshold;
        m_bloomSoftKnee  = softKnee;
    }
    float Exposure() const { return m_exposure; }
    float BloomIntensity() const { return m_bloomIntensity; }

    // Recreate the HDR target at a new size. Returns false if allocation failed,
    // in which case the target is released and the caller must not render.
    bool ResizeHDRTarget(D3D12Device& device, uint32_t width, uint32_t height);

    // ---- Shadow pass -------------------------------------------------------
    // Runs BEFORE BeginScenePass. Renders scene depth from the light's point of
    // view into a dedicated depth target, which the main pass then samples.
    //
    // FOUR cascades, all centred on the camera-relative origin (i.e. the
    // camera), reaching from 24 to ~448 world units. Rebuilt every frame: the
    // light can move, and the frustums are anchored to the camera, which
    // certainly does.
    //
    // Call order per frame is ONE BeginShadowPass, then one
    // BeginShadowCascade + RenderShadowCasters per cascade, then ONE
    // EndShadowPass. The two whole-resource barriers live in Begin/End, so the
    // m_shadowIsDepthTarget bool describes the whole resource and stays
    // coherent; per-slice barriers or an early-out mid-loop would desync it and
    // produce a validation error a long way from its cause.
    //
    // cameraPosition is the absolute Vec3d and is the SANCTIONED RULE 1
    // exception - see core::BuildShadowCascadeMatrix for exactly what it is used
    // for (quantising the texel lattice, in double, never narrowed).
    void BeginShadowPass(D3D12Device& device, const core::Vec3d& cameraPosition);
    // Bind slice `cascade` and clear it. The clear is UNCONDITIONAL and lives
    // here on purpose: D3D12 does not zero-initialise a committed
    // ALLOW_DEPTH_STENCIL resource, so a slice whose clear was skipped can hold
    // arbitrary values below 1.0 and read as "written" without ever having been
    // rendered into - which the coverage probe cannot distinguish. The only way
    // to skip the clear is to skip the whole cascade, and that the smoke markers
    // do catch.
    void BeginShadowCascade(D3D12Device& device, uint32_t cascade);
    void EndShadowPass(D3D12Device& device);

    // Recompute all cascade matrices. Called by BeginShadowPass; exposed so
    // CreateShadowResources can seed valid matrices before the first frame.
    void UpdateShadowCascades(const core::Vec3d& cameraPosition);

    // Depth-only draw. Deliberately a separate entry point rather than a flag on
    // DrawMesh: the shadow pass binds no material, no textures and no per-frame
    // constants, and folding it in would mean a pile of branches on a hot path.
    void DrawMeshShadow(D3D12Device& device, const Mesh& mesh,
                        const core::Mat4x4& worldMatrix);

    bool ShadowsAvailable() const { return m_shadowMap != nullptr; }

    // Smoke-test instrumentation. Without this, deleting the shadow pass call
    // in App::Render breaks nothing that any test or assertion can see: the map
    // stays at its cleared value, every pixel reads fully lit, and the frame
    // still looks plausible. "It compiles" is not "it runs", and this project
    // has been bitten by exactly that gap before.
    //
    // Two phases, matching the back-buffer capture: record the copy into the
    // frame's command list, then read it after the GPU has caught up.
    // Copies the centred probe window of EVERY cascade slice, not just slice 0.
    // Probing only slice 0 would leave cascades 1..3 with no evidence at all
    // that they were ever rasterised.
    bool RecordShadowMapReadback(D3D12Device& device);
    // Fraction of sampled texels holding anything other than the cleared 1.0,
    // and the smallest depth found, for one cascade. A fraction of zero means
    // the depth pass did not rasterise a single triangle into that slice.
    bool ReadShadowMapCoverage(uint32_t cascade,
                               float& writtenFraction,
                               float& minDepth) const;
    // Do the uploaded cascade texel sizes strictly increase with consecutive
    // ratios in (1, 8]? Computed from the table that is ACTUALLY in flight, so
    // it is a check of the live fit rather than a mirror of the constants.
    bool ShadowCascadeTexelSizesAreMonotonic() const;

    // How many cascades BeginShadowCascade was actually called for in the last
    // shadow pass.
    //
    // This exists because the obvious assertion does not work, and that was
    // established empirically rather than assumed. Changing the app's loop bound
    // to skip the last cascade leaves EVERY per-slice coverage marker reading
    // "written=yes": D3D12 does not zero-initialise a committed
    // ALLOW_DEPTH_STENCIL resource, so a slice that was never cleared and never
    // rendered holds arbitrary values below the 1.0 clear and is
    // indistinguishable from a slice full of real depth. The coverage probe
    // physically cannot tell those apart.
    //
    // A CPU-side count of cascades begun is the only cheap thing that can. It
    // does not prove the GPU rasterised anything - that is what the coverage and
    // depth-distinctness markers are for - but it is what catches a loop that
    // stops early.
    uint32_t ShadowCascadesRendered() const { return m_cascadesBegunThisPass; }

    // Set directional light (call before BeginFrame or in init)
    void SetDirectionalLight(const core::Vec3f& direction,
                             const core::Vec3f& color,
                             const core::Vec3f& ambient);

private:
    bool CreateRootSignature(ID3D12Device* device);
    bool CreatePSO(ID3D12Device* device);
    bool CreateSkyPSO(ID3D12Device* device);
    bool CreateConstantBuffers(ID3D12Device* device);
    bool CreateTextureHeap(ID3D12Device* device);
    void WriteNullTextureDescriptor(ID3D12Device* device, uint32_t descriptorIndex);
    bool CreateHDRTarget(ID3D12Device* device, uint32_t width, uint32_t height);
    bool CreateTonemapPipeline(ID3D12Device* device);
    bool CreateBloomPipeline(ID3D12Device* device);
    bool CreateShadowResources(ID3D12Device* device);
    bool CreateShadowPSO(ID3D12Device* device);
    void RenderBloom(D3D12Device& device);

    // Upload a constant buffer and return its GPU virtual address
    D3D12_GPU_VIRTUAL_ADDRESS UploadCB(const void* data, uint32_t dataSize);

    // -------------------------------------------------------------------------
    // Per-frame per-draw structured buffers
    // -------------------------------------------------------------------------
    // These are CPU writes into persistently mapped UPLOAD memory read by the
    // GPU from the command list recorded that frame. NO RESOURCE BARRIER CAN
    // PROTECT THEM: barriers order GPU work, they do not synchronise CPU writes
    // - stated verbatim at path_tracer.h:98-108 and debug_overlay.h:70-73.
    //
    // Fencing is not an alternative either. D3D12Device::WaitForCurrentFrame
    // waits on m_fenceValues[m_frameIndex] only, i.e. the frame kFrameCount-1
    // back that last used this back-buffer index; it does NOT wait for frames
    // N+1 or N+2. kFrameCount-instancing is exactly what makes that narrow wait
    // sufficient.
    //
    // This is the fourth instance of the house FrameUploadBuffer pattern
    // (PathTracer, RTAcceleration, DebugOverlay each carry their own copy).
    // Copied rather than lifted into a shared header on purpose: hoisting it is
    // a mechanical follow-up and folding it in here would widen the blast
    // radius of an already large change.
    struct FrameStructuredBuffer
    {
        ComPtr<ID3D12Resource> buffer[kFrameCount];
        uint8_t*               mapped[kFrameCount] = {};
        uint32_t               capacity = 0;   // ELEMENTS, not bytes

        bool Valid() const { return buffer[0] != nullptr; }

        void Reset()
        {
            for (uint32_t i = 0; i < kFrameCount; ++i)
            {
                if (buffer[i] && mapped[i])
                {
                    buffer[i]->Unmap(0, nullptr);
                    mapped[i] = nullptr;
                }
                buffer[i].Reset();
            }
            capacity = 0;
        }
    };

    bool EnsureFrameStructuredBuffer(D3D12Device& device,
                                     FrameStructuredBuffer& target,
                                     uint32_t elementCount,
                                     uint64_t elementSize,
                                     const wchar_t* debugName);
    bool EnsureDrawProbeResources(D3D12Device& device, uint32_t elementCount);
    bool PrepareDrawProbe(D3D12Device& device);

    FrameStructuredBuffer m_objectBuffer;     // stride sizeof(ObjectData)   = 96
    FrameStructuredBuffer m_materialBuffer;   // stride sizeof(MaterialData) = 80

    // Monotonic across BOTH passes within a frame, reset in BeginFrameResources.
    // The shadow pass occupies object elements [0, N) and the main pass [N, 2N):
    // DISJOINT ranges, not shared records. Sharing was tempting - both passes
    // walk the same pool in the same order with identical filters - but that
    // parity is a coincidence of two loops, enforced nowhere. If shadow casting
    // ever gains a filter the main pass lacks (frustum culling, a castsShadow
    // flag, an LOD cut), shared indices would silently desynchronise and every
    // object would render with a different entity's transform. Disjoint ranges
    // make the two loops matter only for buffer SIZING, where being wrong costs
    // a slightly oversized allocation rather than wrong transforms.
    // The object cursor is a DrawRecordCursor rather than a bare uint32_t so
    // its allocation, pass-marking and per-cascade rewind are production code a
    // CPU-only test can drive directly. See gpu_draw_records.h - the previous
    // "test" for this re-implemented the increments in the test body.
    DrawRecordCursor m_objectCursor;
    uint32_t m_materialCursor = 0;
    // Latched high-water marks and per-pass counts for the smoke markers.
    uint32_t m_objectPeak     = 0;
    uint32_t m_materialPeak   = 0;
    // Live for the frame being recorded: where the shadow pass's slice ended.
    uint32_t m_shadowRecords  = 0;
    // Latched from the last frame that actually rendered through the RASTER
    // path. Path-traced frames run neither pass, so without this distinction an
    // F1 toggle would leave the parity marker reading a half-state - the
    // previous raster frame's shadow count against a main count of zero - and
    // the harness would fail on a run where nothing was wrong.
    uint32_t m_reportedShadowRecords = 0;
    uint32_t m_reportedMainRecords   = 0;
    // Overflow is logged once per run rather than per draw: the harness throws
    // on any [ERR ] line, and one draw over capacity means every subsequent one
    // is too.
    bool     m_drawOverflowLogged = false;
    uint32_t m_structuredBufferReallocations = 0;
    uint32_t m_structuredBufferReallocationsInFlight = 0;
    // Frames that have been through BeginFrameResources, i.e. that recorded a
    // command list binding the per-draw buffers. Used only to classify a grow as
    // hazardous or not - see EnsureFrameStructuredBuffer for why neither the
    // completed-fence value nor the global fence value works here.
    uint32_t m_framesBegun = 0;

    // A UAV written by the raster vertex shaders only when smoke verification
    // is enabled. One DEFAULT buffer per frame slot avoids clearing storage a
    // still-in-flight frame may be writing. The shared zero upload is immutable
    // and may safely feed every slot's copy.
    ComPtr<ID3D12Resource> m_drawProbeBuffer[kFrameCount];
    ComPtr<ID3D12Resource> m_drawProbeZeroUpload;
    ComPtr<ID3D12Resource> m_drawProbeReadback;
    uint32_t m_drawProbeCapacity = 0;
    UINT64 m_drawProbeReadbackBytes = 0;
    uint32_t m_drawProbeReadbackFrame = 0;
    uint32_t m_drawProbeRecordCount = 0;
    uint32_t m_drawProbeShadowRecords = 0;
    uint32_t m_drawProbeMaterialRecords = 0;
    bool m_drawProbeEnabled = false;
    bool m_drawProbeReadbackPending = false;

    // Root signature and PSO
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;

    // The probe permutation of the main PSO: identical state, pixel shader
    // compiled with DAWNING_DRAW_PROBE so it declares the probe UAV. Bound ONLY
    // on the frame the draw-record probe runs.
    //
    // It exists because a pixel shader that declares a UAV loses early-Z for the
    // whole PSO, in every configuration, and no runtime flag can buy that back -
    // `drawProbeEnabled` gates the write, but the DECLARATION is what marks the
    // shader as side-effecting. Keeping the probe on a separate PSO is what lets
    // m_pso stay early-Z-eligible on every ordinary frame while the probe still
    // ships and still runs in Release.
    ComPtr<ID3D12PipelineState> m_psoDrawProbe;

    // The main-pass PSO for THIS frame. Every site that binds the opaque pipeline
    // must go through here, or a probe frame would rasterise with the PSO whose
    // pixel shader cannot write the probe and the material half would read as
    // "unshaded" for every draw.
    ID3D12PipelineState* MainPSO() const
    {
        return m_drawProbeEnabled ? m_psoDrawProbe.Get() : m_pso.Get();
    }

    ComPtr<ID3D12PipelineState> m_skyPSO;

    // HDR scene target. Its own RTV and shader-visible SRV heaps rather than
    // slots in the texture table, so the tone-map pass stays independent of
    // material descriptor allocation.
    static constexpr DXGI_FORMAT kHDRFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr float kSceneClearColor[4] = { 0.50f, 0.55f, 0.62f, 1.0f };
    ComPtr<ID3D12Resource>       m_hdrTarget;
    ComPtr<ID3D12DescriptorHeap> m_hdrRtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_hdrSrvHeap;   // 3: scene, bloom A, bloom B
    uint32_t                     m_hdrWidth = 0;
    uint32_t                     m_hdrHeight = 0;
    // Tracks whether the target is currently a render target or a shader
    // resource, so the pass helpers can emit the right transition without the
    // caller having to know.
    bool                         m_hdrIsRenderTarget = false;

    ComPtr<ID3D12RootSignature> m_tonemapRootSig;
    ComPtr<ID3D12PipelineState> m_tonemapPSO;

    // Bloom ping-pong pair at half resolution, plus the pipelines that drive
    // them. Half res because bloom is a low-frequency effect: full resolution
    // costs 4x the bandwidth for detail the blur immediately destroys.
    //
    // SRV heap layout, which the pass order depends on:
    //   0 = scene HDR   1 = bloom A   2 = bloom B
    // prefilter writes A, horizontal blur reads A writes B, vertical blur reads
    // B writes A. The result therefore lands in A, at descriptor 1, so the
    // tone-map pass can bind the table at descriptor 0 and get t0 = scene,
    // t1 = bloom with no extra copy.
    static constexpr uint32_t kBloomTargetCount = 2;
    ComPtr<ID3D12Resource>       m_bloomTarget[kBloomTargetCount];
    ComPtr<ID3D12DescriptorHeap> m_bloomRtvHeap;   // 2 RTVs
    uint32_t                     m_bloomWidth = 0;
    uint32_t                     m_bloomHeight = 0;
    bool                         m_bloomIsRenderTarget[kBloomTargetCount] = {};

    ComPtr<ID3D12RootSignature> m_bloomRootSig;
    ComPtr<ID3D12PipelineState> m_bloomPrefilterPSO;
    ComPtr<ID3D12PipelineState> m_bloomBlurPSO;

    float m_exposure       = 1.25f;   // matches the old baked-in constant
    float m_bloomIntensity = 1.2f;
    float m_bloomThreshold = 1.0f;
    float m_bloomSoftKnee  = 0.5f;

    // ---- Shadow map --------------------------------------------------------
    // R32_TYPELESS so the same resource can carry a D32_FLOAT DSV for the shadow
    // pass and an R32_FLOAT SRV for the main pass.
    //
    // The SRV lives in m_textureHeap at kShadowDescriptorIndex, NOT in a heap of
    // its own: only one shader-visible CBV_SRV_UAV heap can be bound at a time,
    // so anything the material pass samples has to share that heap. The slot is
    // reserved by starting the descriptor allocator past it, so it can never be
    // handed out to a material texture.
    // Resolution and the cascade geometry now live in core/shadow_cascades.h so
    // the unit tests can reach them without linking any D3D12.
    static constexpr uint32_t kShadowMapSize = core::kShadowMapSize;
    static constexpr uint32_t kShadowDescriptorIndex = 1;  // 0 is the null SRV

    // ONE Texture2DArray with kShadowCascadeCount slices, NOT N separate
    // textures and NOT an atlas.
    //
    // A Texture2DArray SRV is ONE descriptor however many slices it has, so the
    // root signature, the descriptor heap layout and kShadowDescriptorIndex are
    // all completely unchanged by adding cascades - which is why this change
    // does not open CreateRootSignature at all.
    //
    // Not an atlas, for a specific reason: the s1 comparison sampler uses
    // ADDRESS_MODE_BORDER with OPAQUE_WHITE, so a tap outside a cascade's
    // footprint reads as LIT with no branch in the shader. In an atlas,
    // "outside the tile" is still inside the texture, so a 3x3 kernel at a tile
    // edge would silently sample a neighbouring cascade's depth and return a
    // plausible wrong answer instead.
    //
    // 4 * 2048 * 2048 * 4 = 64 MiB, up from 16 MiB.
    ComPtr<ID3D12Resource>       m_shadowMap;
    ComPtr<ID3D12DescriptorHeap> m_shadowDsvHeap;   // kShadowCascadeCount DSVs
    uint32_t                     m_shadowDsvDescSize = 0;
    ComPtr<ID3D12PipelineState>  m_shadowPSO;
    core::Mat4x4                 m_lightViewProj[core::kShadowCascadeCount];
    bool                         m_shadowIsDepthTarget = false;
    // Which slice DrawMeshShadow is currently recording into. Set by
    // BeginShadowCascade; the ONLY thing that selects a cascade's matrix.
    uint32_t                     m_activeCascade = 0;
    // Reset by BeginShadowPass, incremented by BeginShadowCascade. See
    // ShadowCascadesRendered for why a CPU counter is load-bearing here.
    uint32_t                     m_cascadesBegunThisPass = 0;
    // A centred window rather than the whole 2048x2048 map: 1 MiB across four
    // slices instead of 64 MiB. Every cascade is centred on the camera-relative
    // origin, so this is where the geometry lands in all of them.
    static constexpr uint32_t    kShadowProbeSize = 256;
    ComPtr<ID3D12Resource>       m_shadowReadback;
    UINT64                       m_shadowReadbackBytes = 0;

    // ---- Environment IBL ---------------------------------------------------
    // The prefiltered environment cubemap. Its SRV takes ONE reserved slot in
    // m_textureHeap, immediately after the shadow map and by the identical
    // mechanism: a fixed index below the allocator's firstIndex, so it is valid
    // before any material exists and can never be recycled.
    //
    // COST, PLAINLY: usable material slots drop 126 -> 125. That is ~0.25 of a
    // four-map PBR material, taking the practical ceiling from ~31.5 materials
    // to ~31.25. It is the smallest possible bite out of the scarcest budget in
    // the renderer, and it buys the largest missing piece of the lighting model.
    // It does not change the ceiling's character or bring the bindless work
    // forward.
    //
    // NOTHING SAMPLES IT YET. Stage 1 of docs/research/IBL_DESIGN.md builds and
    // proves the resource; Stages 2-4 consume it. See environment_ibl.h.
    static constexpr uint32_t kEnvCubeDescriptorIndex = 2;
    EnvironmentIBL m_environmentIBL;

    // Bumped whenever the sky changes; EnsureEnvironmentIBL rebakes when the
    // cube's baked revision no longer matches. A COUNTER, not a bool, so a
    // dynamic sky is an edit rather than a redesign (IBL_DESIGN.md section 5).
    //
    // NOTE FOR THE DAY THE SKY READS THE LIGHT: Renderer::Init runs before
    // SetDirectionalLight, so prefiltering at Init is correct only while the sky
    // is independent of the light - which it is today, because it has no sun
    // disc. The moment DawningSkyRadiance consults the light direction,
    // SetDirectionalLight MUST bump this counter, or the cube silently bakes the
    // default light direction forever.
    uint32_t m_skyRevision = 0;

    // Shader-visible texture descriptors. Slot 0 is a null SRV fallback,
    // slot 1 the shadow map, slot 2 the environment cube.
    static constexpr uint32_t kMaxRasterTextures = 128;
    ComPtr<ID3D12DescriptorHeap> m_textureHeap;
    uint32_t m_textureDescSize = 0;
    // Replaces a bare monotonic counter. Slot 0 is reserved as the permanent
    // null-SRV fallback, so the allocator starts handing out at 1.
    DescriptorAllocator m_textureAllocator;

    // Per-frame upload buffers for constants (one per frame in flight)
    static constexpr uint32_t kCBRingSize = 256 * 1024; // 256KB per frame
    ComPtr<ID3D12Resource> m_cbUploadBuffers[kFrameCount];
    uint8_t* m_cbMappedPtrs[kFrameCount] = {};
    uint32_t m_cbOffset = 0;       // Current write offset in ring
    uint32_t m_cbPeak = 0;         // Highest offset reached on any frame
    uint32_t m_currentFrame = 0;

    // Guards against a pass being added ABOVE BeginFrameResources in
    // App::RenderFrame, which would silently reintroduce the frame-slot bug that
    // function exists to fix: allocations landing in the previous frame's buffer,
    // past its high-water mark, with root descriptors pointing into storage a
    // still-in-flight frame may be reading. It has no visual symptom until entity
    // counts grow, which is why it needs a guard at all.
    //
    // Checked by RequireFrameResources(), NOT by a bare assert(). An assert is
    // compiled out in Release, and Release is where the higher entity counts that
    // actually trip this get run. See renderer.cpp.
    bool m_frameResourcesBegun = false;
    // Latches the Release-path report so a broken frame logs once, not once per
    // draw call - the harness fails on the first [ERR ] line either way.
    bool m_frameResourcesViolationLogged = false;

    // Returns true when it is legal to record into this frame's arena. In Debug
    // it also asserts, so a debugger stops at the offending call rather than at
    // the log line.
    bool RequireFrameResources(const char* site);

    // Cached view-projection matrix for the current frame
    core::Mat4x4 m_viewProj;
    core::Vec3f  m_eyePos;

    // Directional light
    core::Vec3f m_lightDir = core::Vec3f(0.5f, 0.8f, 0.3f).Normalized();
    core::Vec3f m_lightColor = { 1.0f, 0.98f, 0.95f };
    core::Vec3f m_ambientColor = { 0.15f, 0.17f, 0.25f };
};

} // namespace render
