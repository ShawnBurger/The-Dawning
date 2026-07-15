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
};

struct CBMaterial
{
    float albedo[4];     // RGBA
    float roughness;
    float metallic;
    uint32_t useAlbedoTexture;
    uint32_t useNormalTexture;
    uint32_t albedoTextureIndex;
    uint32_t normalTextureIndex;
};

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
                  const Texture* normalTexture = nullptr);

    // Register a texture SRV for raster material sampling.
    uint32_t RegisterTexture(ID3D12Device* device, const Texture& texture);

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

    // Upload a constant buffer and return its GPU virtual address
    D3D12_GPU_VIRTUAL_ADDRESS UploadCB(const void* data, uint32_t dataSize);

    // Root signature and PSO
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3D12PipelineState> m_skyPSO;

    // Shader-visible texture descriptors. Slot 0 is a null SRV fallback.
    static constexpr uint32_t kMaxRasterTextures = 128;
    ComPtr<ID3D12DescriptorHeap> m_textureHeap;
    uint32_t m_textureDescSize = 0;
    uint32_t m_nextTextureDescriptor = 1;

    // Per-frame upload buffers for constants (one per frame in flight)
    static constexpr uint32_t kCBRingSize = 256 * 1024; // 256KB per frame
    ComPtr<ID3D12Resource> m_cbUploadBuffers[kFrameCount];
    uint8_t* m_cbMappedPtrs[kFrameCount] = {};
    uint32_t m_cbOffset = 0;       // Current write offset in ring
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
