#pragma once
// =============================================================================
// render/path_tracer.h — Full Path Tracer Orchestrator
// =============================================================================
// Manages the complete path tracing pipeline:
//   - RT output UAV texture
//   - Per-frame constant buffer upload
//   - Material buffer upload
//   - DispatchRays execution
//   - Copy RT output to back buffer (until DLSS is integrated)
//
// Future: ReSTIR DI/GI, SER, DLSS Ray Reconstruction, denoising
// =============================================================================

#include "rt_pipeline.h"
#include "rt_acceleration.h"
#include "camera.h"
#include "d3d12_device.h"
#include "texture.h"
#include "../core/types.h"
#include <array>
#include <cstdint>

namespace render
{

enum class RTQualityMode : uint32_t
{
    StablePreview = 0,
    FullPathTrace = 1
};

struct RTQualityInfo
{
    const char* name = "Stable Preview";
    const char* shortName = "STABLE";
    uint32_t samplesPerPixel = 8;
    uint32_t maxBounces = 1;
    uint32_t stablePreview = 1;
};

RTQualityInfo GetRTQualityInfo(RTQualityMode mode);

class PathTracer
{
public:
    bool Init(D3D12Device& device);
    void Shutdown();

    // Resize the RT output texture (call on window resize)
    void Resize(ID3D12Device5* device, uint32_t width, uint32_t height);

    // Access to acceleration structures and pipeline for scene to build
    RTAcceleration& GetAcceleration() { return m_accel; }
    RTPipeline&     GetPipeline()     { return m_pipeline; }

    // Dispatch path tracing for the current frame
    void Dispatch(D3D12Device& device,
                  const Camera& camera,
                  const core::Vec3f& lightDir,
                  const core::Vec3f& lightColor,
                  const core::Vec3f& ambientColor,
                  const RTMaterialData* materials,
                  uint32_t materialCount,
                  const RTInstanceData* instanceData,
                  uint32_t instanceDataCount,
                  const RTTriangleNormalData* triangleNormals,
                  uint32_t triangleNormalCount,
                  const RTTriangleUVData* triangleUVs,
                  uint32_t triangleUVCount,
                  const Texture* const* albedoTextures,
                  uint32_t albedoTextureCount,
                  uint32_t instanceCount,
                  RTQualityMode qualityMode);

    // Copy the RT output to the back buffer for display
    void CopyToBackBuffer(D3D12Device& device);

    bool IsInitialized() const { return m_initialized; }

private:
    bool CreateOutputTexture(ID3D12Device5* device, uint32_t width, uint32_t height);
    bool CreateDescriptorHeap(ID3D12Device5* device);
    bool CreateConstantBuffer(ID3D12Device5* device);
    bool CreateMaterialBuffer(ID3D12Device5* device, uint32_t maxMaterials);
    bool EnsureInstanceDataBuffer(ID3D12Device5* device, uint32_t instanceCount);
    bool EnsureTriangleNormalBuffer(ID3D12Device5* device, uint32_t triangleCount);
    bool EnsureTriangleUVBuffer(ID3D12Device5* device, uint32_t triangleCount);
    void ClearAlbedoTextureDescriptors(ID3D12Device5* device);
    uint32_t UpdateAlbedoTextureDescriptors(ID3D12Device5* device,
                                            const Texture* const* textures,
                                            uint32_t textureCount);

    RTAcceleration m_accel;
    RTPipeline     m_pipeline;

    // RT output textures: HDR history for accumulation, 8-bit display for copy.
    ComPtr<ID3D12Resource> m_outputTexture;
    ComPtr<ID3D12Resource> m_displayTexture;
    uint32_t m_outputWidth = 0;
    uint32_t m_outputHeight = 0;

    // Descriptor heap for UAV
    ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;
    uint32_t m_srvUavDescSize = 0;
    uint32_t m_boundAlbedoTextureCount = 0;
    std::array<ID3D12Resource*, kMaxRTAlbedoTextures> m_boundAlbedoTextureResources = {};

    // Per-frame constant buffer (upload heap, persistently mapped)
    ComPtr<ID3D12Resource> m_constantBuffer[3]; // One per frame in flight
    uint8_t* m_cbMapped[3] = {};

    // Material structured buffer (upload heap)
    ComPtr<ID3D12Resource> m_materialBuffer;
    uint8_t* m_materialMapped = nullptr;
    uint32_t m_maxMaterials = 0;

    // Geometry metadata consumed by closest-hit shaders
    ComPtr<ID3D12Resource> m_instanceDataBuffer;
    uint8_t* m_instanceDataMapped = nullptr;
    uint32_t m_maxInstanceData = 0;

    ComPtr<ID3D12Resource> m_triangleNormalBuffer;
    uint8_t* m_triangleNormalMapped = nullptr;
    uint32_t m_maxTriangleNormals = 0;

    ComPtr<ID3D12Resource> m_triangleUVBuffer;
    uint8_t* m_triangleUVMapped = nullptr;
    uint32_t m_maxTriangleUVs = 0;

    core::Vec3f m_prevCameraPos = {};
    core::Vec3f m_prevCameraRight = {};
    core::Vec3f m_prevCameraUp = {};
    core::Vec3f m_prevCameraForward = {};
    RTQualityMode m_prevQualityMode = RTQualityMode::StablePreview;
    uint32_t m_prevSamplesPerPixel = 0;
    uint32_t m_prevMaxBounces = 0;
    uint32_t m_accumFrameIndex = 0;
    uint32_t m_frameIndex = 0;
    bool     m_hasPrevCamera = false;
    bool     m_hasPrevQuality = false;
    bool     m_initialized = false;
};

} // namespace render
