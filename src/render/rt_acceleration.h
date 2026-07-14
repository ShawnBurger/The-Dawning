#pragma once
// =============================================================================
// render/rt_acceleration.h — DXR Acceleration Structure Management
// =============================================================================
// Manages BLAS (per-mesh) and TLAS (per-scene, rebuilt every frame).
//
// BLAS: built from existing Mesh vertex/index buffers with PREFER_FAST_TRACE
//       + ALLOW_COMPACTION for static geometry.
// TLAS: rebuilt every frame from instance descriptors (moving objects).
//
// Based on NVIDIA RTX best practices and the DXR functional spec.
// =============================================================================

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include "mesh.h"

using Microsoft::WRL::ComPtr;

namespace render
{

// =============================================================================
// BLASEntry — one built bottom-level acceleration structure
// =============================================================================
struct BLASEntry
{
    ComPtr<ID3D12Resource> result;       // Final BLAS buffer (VRAM)
    ComPtr<ID3D12Resource> scratch;      // Kept alive until compaction/lifetime policy exists
    uint64_t               gpuAddress = 0;
    uint32_t               meshIndex = UINT32_MAX; // Index into resource manager
    bool                   compacted = false;
};

// =============================================================================
// TLASInstance — per-entity instance data for TLAS
// =============================================================================
struct TLASInstance
{
    float    transform[3][4];             // Row-major 3x4 instance-to-world
    uint32_t instanceID = 0;              // InstanceID() in shader
    uint8_t  instanceMask = 0xFF;         // Visibility mask
    uint32_t hitGroupOffset = 0;          // SBT hit group offset
    uint32_t instanceFlags = 0;           // D3D12_RAYTRACING_INSTANCE_FLAGS
    uint64_t blasAddress = 0;             // GPU VA of BLAS
};

// =============================================================================
// RTAcceleration — manages BLAS pool and per-frame TLAS
// =============================================================================
class RTAcceleration
{
public:
    bool Init(ID3D12Device5* device);
    void Shutdown();

    // Build a BLAS from an existing GPU mesh. Returns index into BLAS pool.
    uint32_t BuildBLAS(ID3D12Device5* device,
                       ID3D12GraphicsCommandList4* cmdList,
                       const Mesh& mesh);

    // Rebuild TLAS from a set of instances. Call every frame.
    bool BuildTLAS(ID3D12Device5* device,
                   ID3D12GraphicsCommandList4* cmdList,
                   const TLASInstance* instances,
                   uint32_t instanceCount);

    // Access
    ID3D12Resource* GetTLAS() const { return m_tlasResult.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetTLASAddress() const
    {
        return m_tlasResult ? m_tlasResult->GetGPUVirtualAddress() : 0;
    }

    const BLASEntry& GetBLAS(uint32_t index) const { return m_blasPool[index]; }
    uint32_t BLASCount() const { return static_cast<uint32_t>(m_blasPool.size()); }

private:
    ComPtr<ID3D12Resource> CreateUAVBuffer(ID3D12Device* device, uint64_t size,
                                            D3D12_RESOURCE_STATES initialState,
                                            const wchar_t* name = nullptr);

    std::vector<BLASEntry> m_blasPool;

    // TLAS resources (reused across frames)
    ComPtr<ID3D12Resource> m_tlasResult;
    ComPtr<ID3D12Resource> m_tlasScratch;
    ComPtr<ID3D12Resource> m_tlasInstanceBuffer; // Upload heap for instance descs
    uint8_t*               m_tlasInstanceMapped = nullptr;
    uint32_t               m_tlasMaxInstances = 0;
    uint64_t               m_tlasResultSize = 0;
    uint64_t               m_tlasScratchSize = 0;
};

} // namespace render
