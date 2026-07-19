// =============================================================================
// render/rt_acceleration.cpp — Acceleration Structure Implementation
// =============================================================================

#include "rt_acceleration.h"
#include "../core/log.h"
#include <cstring>
#include <cstdint>

namespace render
{

// =============================================================================
// Helper: create a UAV-capable buffer
// =============================================================================
ComPtr<ID3D12Resource> RTAcceleration::CreateUAVBuffer(
    ID3D12Device* device, uint64_t size,
    D3D12_RESOURCE_STATES initialState, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = size;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, initialState, nullptr,
        IID_PPV_ARGS(&resource));

    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create UAV buffer (size=%llu): 0x%08X",
                          static_cast<unsigned long long>(size), hr);
        return nullptr;
    }

    if (name) resource->SetName(name);
    return resource;
}

// =============================================================================
// Init / Shutdown
// =============================================================================
bool RTAcceleration::Init(ID3D12Device5* device)
{
    // Check DXR support
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                              &options5, sizeof(options5));
    if (FAILED(hr) || options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
    {
        core::Log::Error("DXR not supported on this device");
        return false;
    }

    const char* tierStr = options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1
                          ? "1.1" : "1.0";
    core::Log::Infof("DXR Tier %s supported", tierStr);

    m_blasPool.reserve(64);
    core::Log::Info("RTAcceleration initialized");
    return true;
}

void RTAcceleration::Shutdown()
{
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_tlasInstanceBuffer[i] && m_tlasInstanceMapped[i])
        {
            m_tlasInstanceBuffer[i]->Unmap(0, nullptr);
            m_tlasInstanceMapped[i] = nullptr;
        }
        m_tlasInstanceBuffer[i].Reset();
        m_tlasResult[i].Reset();
        m_tlasScratch[i].Reset();
    }

    m_blasPool.clear();
    core::Log::Info("RTAcceleration shut down");
}

// =============================================================================
// BuildBLAS — build from existing mesh vertex/index buffers
// =============================================================================
uint32_t RTAcceleration::BuildBLAS(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* cmdList,
    const Mesh& mesh)
{
    if (!mesh.IsValid())
    {
        core::Log::Error("BuildBLAS: invalid mesh");
        return UINT32_MAX;
    }

    // Describe the geometry
    D3D12_RAYTRACING_GEOMETRY_DESC geomDesc = {};
    geomDesc.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE; // All opaque for now

    geomDesc.Triangles.VertexBuffer.StartAddress  = mesh.vbView.BufferLocation;
    geomDesc.Triangles.VertexBuffer.StrideInBytes  = mesh.vbView.StrideInBytes;
    geomDesc.Triangles.VertexCount                 = mesh.vertexCount;
    geomDesc.Triangles.VertexFormat                = DXGI_FORMAT_R32G32B32_FLOAT;

    geomDesc.Triangles.IndexBuffer  = mesh.ibView.BufferLocation;
    geomDesc.Triangles.IndexCount   = mesh.indexCount;
    geomDesc.Triangles.IndexFormat  = mesh.indexFormat;
    geomDesc.Triangles.Transform3x4 = 0; // No per-geometry transform

    // Get prebuild info
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs       = 1;
    inputs.pGeometryDescs = &geomDesc;
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    // Allocate result and scratch buffers
    auto resultBuffer = CreateUAVBuffer(device, prebuild.ResultDataMaxSizeInBytes,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"BLAS_Result");
    auto scratchBuffer = CreateUAVBuffer(device, prebuild.ScratchDataSizeInBytes,
        D3D12_RESOURCE_STATE_COMMON, L"BLAS_Scratch");

    if (!resultBuffer || !scratchBuffer)
    {
        core::Log::Error("BuildBLAS: failed to allocate buffers");
        return UINT32_MAX;
    }

    // Build the BLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs                           = inputs;
    buildDesc.DestAccelerationStructureData    = resultBuffer->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = scratchBuffer->GetGPUVirtualAddress();

    cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // UAV barrier — BLAS must complete before TLAS build or TraceRay
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resultBuffer.Get();
    cmdList->ResourceBarrier(1, &barrier);

    // Store in pool
    BLASEntry entry;
    entry.result     = std::move(resultBuffer);
    entry.scratch    = std::move(scratchBuffer);
    entry.gpuAddress = entry.result->GetGPUVirtualAddress();

    uint32_t index = static_cast<uint32_t>(m_blasPool.size());
    m_blasPool.push_back(std::move(entry));

    core::Log::Infof("BLAS built: index=%u verts=%u tris=%u size=%llu bytes",
                     index, mesh.vertexCount, mesh.indexCount / 3,
                     static_cast<unsigned long long>(prebuild.ResultDataMaxSizeInBytes));

    // Scratch is retained with the BLAS for now. Releasing it immediately after
    // recording the build can invalidate GPU work before the command list runs.
    return index;
}

// =============================================================================
// BuildTLAS — rebuild every frame from instance descriptors
// =============================================================================
bool RTAcceleration::BuildTLAS(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* cmdList,
    const TLASInstance* instances,
    uint32_t instanceCount)
{
    if (instanceCount == 0) return true;

    // Advance first, so GetTLAS/GetTLASAddress below and in PathTracer::Dispatch
    // all name the slot this call is about to write.
    m_frameSlot = (m_frameSlot + 1) % kFrameCount;

    const uint64_t instanceDescSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC);

    // Grow instance upload buffers if needed - all slots together, since they
    // share one capacity.
    if (instanceCount > m_tlasMaxInstances)
    {
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (m_tlasInstanceBuffer[i] && m_tlasInstanceMapped[i])
            {
                m_tlasInstanceBuffer[i]->Unmap(0, nullptr);
                m_tlasInstanceMapped[i] = nullptr;
            }
            m_tlasInstanceBuffer[i].Reset();
        }

        const uint32_t newMax = instanceCount + 64; // headroom
        const uint64_t newSize = instanceDescSize * newMax;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = newSize;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = { 1, 0 };
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            HRESULT hr = device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&m_tlasInstanceBuffer[i]));
            if (FAILED(hr))
            {
                core::Log::Errorf("BuildTLAS: failed to create instance buffer %u: 0x%08X", i, hr);
                m_tlasMaxInstances = 0;
                return false;
            }
            wchar_t name[48];
            swprintf_s(name, L"TLAS_InstanceBuffer[%u]", i);
            m_tlasInstanceBuffer[i]->SetName(name);

            D3D12_RANGE readRange = { 0, 0 };
            hr = m_tlasInstanceBuffer[i]->Map(0, &readRange,
                                              reinterpret_cast<void**>(&m_tlasInstanceMapped[i]));
            if (FAILED(hr) || !m_tlasInstanceMapped[i])
            {
                // Do NOT commit m_tlasMaxInstances on failure: a later call would
                // skip this branch and write through a null pointer, permanently.
                core::Log::Errorf("BuildTLAS: failed to map instance buffer %u: 0x%08X", i, hr);
                m_tlasInstanceMapped[i] = nullptr;
                m_tlasInstanceBuffer[i].Reset();
                m_tlasMaxInstances = 0;
                return false;
            }
        }
        m_tlasMaxInstances = newMax;
    }

    if (!m_tlasInstanceMapped[m_frameSlot])
    {
        core::Log::Error("BuildTLAS: instance buffer is not mapped");
        return false;
    }

    // Fill instance descriptors
    auto* dstDescs = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(m_tlasInstanceMapped[m_frameSlot]);
    for (uint32_t i = 0; i < instanceCount; i++)
    {
        auto& dst = dstDescs[i];
        const auto& src = instances[i];

        // Copy 3x4 transform (row-major, same as D3D12 expects)
        memcpy(dst.Transform, src.transform, sizeof(float) * 12);

        dst.InstanceID                          = src.instanceID;
        dst.InstanceMask                        = src.instanceMask;
        dst.InstanceContributionToHitGroupIndex = src.hitGroupOffset;
        dst.Flags                               = src.instanceFlags;
        dst.AccelerationStructure               = src.blasAddress;
    }

    // Get prebuild info for TLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs      = instanceCount;
    inputs.InstanceDescs = m_tlasInstanceBuffer[m_frameSlot]->GetGPUVirtualAddress();
    inputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    // Allocate/grow TLAS buffers if needed
    if (prebuild.ResultDataMaxSizeInBytes > m_tlasResultSize)
    {
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            wchar_t name[40];
            swprintf_s(name, L"TLAS_Result[%u]", i);
            m_tlasResult[i] = CreateUAVBuffer(device, prebuild.ResultDataMaxSizeInBytes,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, name);
        }
        m_tlasResultSize = prebuild.ResultDataMaxSizeInBytes;
    }

    if (prebuild.ScratchDataSizeInBytes > m_tlasScratchSize)
    {
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            wchar_t name[40];
            swprintf_s(name, L"TLAS_Scratch[%u]", i);
            m_tlasScratch[i] = CreateUAVBuffer(device, prebuild.ScratchDataSizeInBytes,
                D3D12_RESOURCE_STATE_COMMON, name);
        }
        m_tlasScratchSize = prebuild.ScratchDataSizeInBytes;
    }

    if (!m_tlasResult[m_frameSlot] || !m_tlasScratch[m_frameSlot])
    {
        core::Log::Error("BuildTLAS: failed to allocate TLAS buffers");
        return false;
    }

    // Build TLAS
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs                           = inputs;
    buildDesc.DestAccelerationStructureData    = m_tlasResult[m_frameSlot]->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = m_tlasScratch[m_frameSlot]->GetGPUVirtualAddress();

    cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // UAV barrier — TLAS must complete before TraceRay
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_tlasResult[m_frameSlot].Get();
    cmdList->ResourceBarrier(1, &barrier);

    return true;
}

} // namespace render
