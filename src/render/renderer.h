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
// Constant buffer layout (matches shaders):
//   b0: CBPerObject  — worldViewProj, world, worldInvTranspose (192 bytes → 256 aligned)
//   b1: CBPerFrame   — lightDir, lightColor, ambient, eyePos (64 bytes → 256 aligned)
//   b2: CBMaterial    — albedo, roughness, metallic, texture indices
// =============================================================================

#include "d3d12_device.h"
#include "descriptor_allocator.h"
#include "mesh.h"
#include "camera.h"
#include "texture.h"
#include "../core/types.h"
#include "../core/shadow_cascades.h"
#include <cstddef>
#include <cstdint>

namespace render
{

// =============================================================================
// Constant buffer structs (must match HLSL cbuffer layouts exactly)
// =============================================================================
struct CBPerObject
{
    float worldViewProj[16];
    float world[16];
    float worldInvTranspose[16];
};

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

struct CBMaterial
{
    float albedo[4];     // RGBA
    float roughness;
    float metallic;
    uint32_t useAlbedoTexture;
    uint32_t useNormalTexture;
    uint32_t albedoTextureIndex;
    uint32_t normalTextureIndex;
    uint32_t useOrmTexture;
    uint32_t ormTextureIndex;
    float emissive[3];
    float emissiveStrength;
    uint32_t useEmissiveTexture;
    uint32_t emissiveTextureIndex;
    uint32_t cbMaterialPad0;
    uint32_t cbMaterialPad1;
};
static_assert(sizeof(CBMaterial) == 80,
              "CBMaterial must match cbuffer CBMaterial (b2) in basic_ps.hlsl");

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

    // Advance the per-frame constant ring. MUST run exactly once per frame,
    // BEFORE any pass uploads constants.
    //
    // This used to live inside BeginFrame, which was wrong the moment a pass
    // started running earlier than BeginFrame. The shadow pass does exactly
    // that: it runs before BeginScenePass, which runs before BeginFrame, so its
    // per-object constants were allocated against the PREVIOUS frame's index
    // and appended past that frame's high-water mark. It did not corrupt
    // anything only because the regions happened to be disjoint and three
    // frames in flight gave enough fence slack - an accident of the current
    // allocation pattern, not a property anything enforced.
    //
    // Called for both the raster and path-tracing branches so the ring is
    // advanced uniformly regardless of which one runs.
    void BeginFrameResources(D3D12Device& device);

    // Peak bytes used in the constant ring on any frame so far. Exposed because
    // the ring is a fixed kCBRingSize and every new per-draw pass multiplies
    // pressure on it; a silent overflow degrades into dropped draws.
    uint32_t ConstantRingPeakBytes() const { return m_cbPeak; }
    uint32_t ConstantRingCapacity() const { return kCBRingSize; }

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
    uint32_t m_cbPeak = 0;         // Highest offset reached on any frame
    uint32_t m_currentFrame = 0;

    // Cached view-projection matrix for the current frame
    core::Mat4x4 m_viewProj;
    core::Vec3f  m_eyePos;

    // Directional light
    core::Vec3f m_lightDir = core::Vec3f(0.5f, 0.8f, 0.3f).Normalized();
    core::Vec3f m_lightColor = { 1.0f, 0.98f, 0.95f };
    core::Vec3f m_ambientColor = { 0.15f, 0.17f, 0.25f };
};

} // namespace render
