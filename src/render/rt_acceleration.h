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
#include "d3d12_device.h"   // kFrameCount

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
    // Both refer to the slot BuildTLAS most recently wrote, so they must be
    // called after BuildTLAS within the same frame.
    ID3D12Resource* GetTLAS() const { return m_tlasResult[m_frameSlot].Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetTLASAddress() const
    {
        return m_tlasResult[m_frameSlot]
             ? m_tlasResult[m_frameSlot]->GetGPUVirtualAddress() : 0;
    }

    const BLASEntry& GetBLAS(uint32_t index) const { return m_blasPool[index]; }
    uint32_t BLASCount() const { return static_cast<uint32_t>(m_blasPool.size()); }

private:
    ComPtr<ID3D12Resource> CreateUAVBuffer(ID3D12Device* device, uint64_t size,
                                            D3D12_RESOURCE_STATES initialState,
                                            const wchar_t* name = nullptr);

    std::vector<BLASEntry> m_blasPool;

    // -------------------------------------------------------------------------
    // TLAS resources, one set per frame in flight
    // -------------------------------------------------------------------------
    // The TLAS is rebuilt from scratch every frame. A single set was only safe
    // because each path-traced frame ends with a full WaitForGpu: the instance
    // buffer is CPU-written per frame (the same mapped-memory race the upload
    // buffers had), while the result and scratch buffers are GPU-written by
    // BuildRaytracingAccelerationStructure - so with frames in flight, frame
    // N+1's build would overwrite the structure frame N is still tracing against.
    // A UAV barrier does not help: it orders work within a frame, not across.
    //
    // m_frameSlot is an internal round-robin rather than the device frame index,
    // deliberately. BuildTLAS runs at most once per device frame, so the slot
    // advances no faster than the frame counter; if RT is toggled off for a
    // while it advances SLOWER, which only increases the gap before reuse. That
    // keeps this class independent of the device's frame index without weakening
    // the guarantee.
    ComPtr<ID3D12Resource> m_tlasResult[kFrameCount];
    ComPtr<ID3D12Resource> m_tlasScratch[kFrameCount];
    ComPtr<ID3D12Resource> m_tlasInstanceBuffer[kFrameCount];
    uint8_t*               m_tlasInstanceMapped[kFrameCount] = {};
    uint32_t               m_frameSlot = 0;
    uint32_t               m_tlasMaxInstances = 0;
    uint64_t               m_tlasResultSize = 0;
    uint64_t               m_tlasScratchSize = 0;
};

} // namespace render
