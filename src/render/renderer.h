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
//   b3:        CBDrawIndex  — { objectIndex, materialIndex }, root 32-bit constants
//   b4:        CBPerPass    — viewProj; light matrix in the shadow pass, camera
//                             matrix in the main pass. Once per pass, not per draw.
//   b0 and b2 are permanently free: they were CBPerObject and CBMaterial, and
//   the new registers are deliberately NOT reused so a diff cannot confuse
//   "b0 is per-object data" with "b0 is a draw index".
// =============================================================================

#include "d3d12_device.h"
#include "gpu_draw_records.h"   // ObjectData / MaterialData / CBPerPass layouts
#include "descriptor_allocator.h"
#include "mesh.h"
#include "camera.h"
#include "texture.h"
#include "../core/types.h"
#include <cstdint>

namespace render
{

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

    // Camera-relative light view-projection, so the pixel shader can project a
    // world position into shadow-map space. Camera-relative because every world
    // matrix the raster path sees already is - see RULES #1 in CLAUDE.md. A
    // light matrix built in absolute world space would be correct only while the
    // camera sat at the origin.
    float lightViewProj[16];
};
static_assert(sizeof(CBPerFrame) == 176,
              "CBPerFrame must match cbuffer CBPerFrame (b1) in the raster shaders");

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
    void BeginFrameResources(D3D12Device& device, uint32_t maxDrawsHint);

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
    uint32_t ShadowRecords() const { return m_reportedShadowRecords; }
    uint32_t MainRecords() const { return m_reportedMainRecords; }

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
    // The light matrix is rebuilt every frame around the camera-relative origin
    // (i.e. the camera), so the map follows the viewer. That is a single
    // cascade covering kShadowExtent units - fine for the demo scene, and the
    // obvious place to add cascades later.
    void BeginShadowPass(D3D12Device& device);
    void EndShadowPass(D3D12Device& device);

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
    bool RecordShadowMapReadback(D3D12Device& device);
    // Fraction of sampled texels holding anything other than the cleared 1.0,
    // and the smallest depth found. A fraction of zero means the depth pass did
    // not rasterise a single triangle.
    bool ReadShadowMapCoverage(float& writtenFraction, float& minDepth) const;

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
    void UpdateLightMatrix();
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
    uint32_t m_objectCursor   = 0;
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

    // Root signature and PSO
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
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
    static constexpr uint32_t kShadowMapSize = 2048;
    static constexpr uint32_t kShadowDescriptorIndex = 1;  // 0 is the null SRV
    // Half-extent of the orthographic light frustum, in world units, centred on
    // the camera. Sized for the demo scene; a real world needs cascades.
    static constexpr float    kShadowExtent = 24.0f;
    static constexpr float    kShadowDepthRange = 120.0f;
    ComPtr<ID3D12Resource>       m_shadowMap;
    ComPtr<ID3D12DescriptorHeap> m_shadowDsvHeap;
    ComPtr<ID3D12PipelineState>  m_shadowPSO;
    core::Mat4x4                 m_lightViewProj;
    bool                         m_shadowIsDepthTarget = false;
    // A centred window rather than the whole 2048x2048 map: 256 KB instead of
    // 16 MB, and the light frustum is centred on the camera so this is where
    // the geometry lands.
    static constexpr uint32_t    kShadowProbeSize = 256;
    ComPtr<ID3D12Resource>       m_shadowReadback;

    // Shader-visible texture descriptors. Slot 0 is a null SRV fallback,
    // slot 1 the shadow map.
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
    uint32_t m_currentFrame = 0;
    // High-water mark across the process lifetime, NOT per frame: the harness
    // samples it once at shutdown, and a per-frame value would report whatever
    // the last frame happened to cost rather than the worst case.
    uint32_t m_cbPeak = 0;
    // Guards against a future pass being added ABOVE BeginFrameResources in
    // App::RenderFrame, which would silently reintroduce exactly the frame-slot
    // bug that function exists to fix. Without it the reintroduction has no
    // symptom until entity counts grow.
    bool m_frameResourcesBegun = false;

    // Cached view-projection matrix for the current frame
    core::Mat4x4 m_viewProj;
    core::Vec3f  m_eyePos;

    // Directional light
    core::Vec3f m_lightDir = core::Vec3f(0.5f, 0.8f, 0.3f).Normalized();
    core::Vec3f m_lightColor = { 1.0f, 0.98f, 0.95f };
    core::Vec3f m_ambientColor = { 0.15f, 0.17f, 0.25f };
};

} // namespace render
