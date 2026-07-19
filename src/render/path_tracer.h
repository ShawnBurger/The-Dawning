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
    // Returns false if the output textures could not be recreated. On failure the
    // path tracer holds no textures and Dispatch() becomes a no-op until a later
    // resize succeeds; callers should fall back to raster.
    bool Resize(ID3D12Device5* device, uint32_t width, uint32_t height);

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
                  const RTTrianglePositionData* trianglePositions,
                  uint32_t trianglePositionCount,
                  const Texture* const* albedoTextures,
                  uint32_t albedoTextureCount,
                  const Texture* const* normalTextures,
                  uint32_t normalTextureCount,
                  const Texture* const* ormTextures,
                  uint32_t ormTextureCount,
                  uint32_t instanceCount,
                  uint64_t sceneSignature,
                  RTQualityMode qualityMode);

    // Copy the RT output to the back buffer for display
    void CopyToBackBuffer(D3D12Device& device);

    bool IsInitialized() const { return m_initialized; }
    uint32_t AccumulationFrameIndex() const { return m_accumFrameIndex; }

private:
    // -------------------------------------------------------------------------
    // Per-frame RT upload buffers
    // -------------------------------------------------------------------------
    // Every one of these is rewritten by the CPU on each dispatch and read by the
    // GPU from the command list recorded that frame. They were single-instanced
    // against kFrameCount frames in flight, which is the same defect the debug
    // overlay had: resource barriers order GPU work, they do not synchronise CPU
    // writes to persistently mapped memory, so no barrier can protect them.
    //
    // It was invisible only because the frame loop ends every path-traced frame
    // with a full WaitForGpu(), serialising the GPU to zero frames in flight.
    // That stall masked the race rather than fixing it, and capped RT throughput
    // permanently. Instancing per frame is the prerequisite for removing it.
    //
    // The constant buffer above was already correct; these five were not.
    struct FrameUploadBuffer
    {
        ComPtr<ID3D12Resource> buffer[kFrameCount];
        uint8_t*               mapped[kFrameCount] = {};
        uint32_t               capacity = 0;   // elements, not bytes

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


    bool CreateOutputTexture(ID3D12Device5* device, uint32_t width, uint32_t height);
    bool CreateDescriptorHeap(ID3D12Device5* device);
    bool CreateConstantBuffer(ID3D12Device5* device);
    // One growth path for all five per-frame upload buffers. Allocates
    // kFrameCount instances so each frame in flight writes its own copy.
    bool EnsureFrameUploadBuffer(D3D12Device& device,
                                 FrameUploadBuffer& target,
                                 uint32_t elementCount,
                                 uint64_t elementSize,
                                 const wchar_t* debugName);
    void ClearMaterialTextureDescriptors(ID3D12Device5* device);
    uint32_t UpdateTextureDescriptors(ID3D12Device5* device,
                                      const Texture* const* textures,
                                      uint32_t textureCount,
                                      uint32_t firstDescriptor,
                                      uint32_t maxDescriptors,
                                      uint32_t& boundCount,
                                      ID3D12Resource** boundResources);

    RTAcceleration m_accel;
    RTPipeline     m_pipeline;

    // RT output textures: HDR history for accumulation, 8-bit display for copy.
    ComPtr<ID3D12Resource> m_outputTexture;
    ComPtr<ID3D12Resource> m_displayTexture;
    uint32_t m_outputWidth = 0;
    uint32_t m_outputHeight = 0;

    // Descriptor heap for UAV
    ComPtr<ID3D12DescriptorHeap> m_srvUavHeap[kFrameCount];
    uint32_t m_srvUavDescSize = 0;
    uint32_t m_boundAlbedoTextureCount[kFrameCount] = {};
    std::array<ID3D12Resource*, kMaxRTAlbedoTextures>
        m_boundAlbedoTextureResources[kFrameCount] = {};
    uint32_t m_boundNormalTextureCount[kFrameCount] = {};
    std::array<ID3D12Resource*, kMaxRTNormalTextures>
        m_boundNormalTextureResources[kFrameCount] = {};
    uint32_t m_boundOrmTextureCount[kFrameCount] = {};
    std::array<ID3D12Resource*, kMaxRTOrmTextures>
        m_boundOrmTextureResources[kFrameCount] = {};

    // Per-frame constant buffer (upload heap, persistently mapped)
    ComPtr<ID3D12Resource> m_constantBuffer[3]; // One per frame in flight
    uint8_t* m_cbMapped[3] = {};

    FrameUploadBuffer m_materialBuffers;
    FrameUploadBuffer m_instanceDataBuffers;
    FrameUploadBuffer m_triangleNormalBuffers;
    FrameUploadBuffer m_triangleUVBuffers;
    FrameUploadBuffer m_trianglePositionBuffers;

    core::Vec3d m_prevCameraPos = {};
    core::Vec3f m_prevCameraRight = {};
    core::Vec3f m_prevCameraUp = {};
    core::Vec3f m_prevCameraForward = {};
    RTQualityMode m_prevQualityMode = RTQualityMode::StablePreview;
    uint32_t m_prevSamplesPerPixel = 0;
    uint32_t m_prevMaxBounces = 0;
    uint64_t m_prevSceneSignature = 0;
    uint32_t m_accumFrameIndex = 0;   // Resets when the rendered view changes; drives accumulation
    uint32_t m_seedFrameCounter = 0;  // Never resets — drives RNG decorrelation
    uint32_t m_frameIndex = 0;        // Frame-in-flight slot (0..kFrameCount-1)
    bool     m_hasPrevCamera = false;
    bool     m_hasPrevQuality = false;
    bool     m_hasPrevScene = false;
    bool     m_initialized = false;
};

} // namespace render
