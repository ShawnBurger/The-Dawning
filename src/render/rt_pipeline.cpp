// =============================================================================
// render/rt_pipeline.cpp — DXR Pipeline Implementation
// =============================================================================

#include "rt_pipeline.h"
#include "shader_utils.h"
#include "../core/log.h"
#include <cstring>
#include <vector>

namespace render
{

// Alignment helpers
static uint64_t Align(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// =============================================================================
// Init / Shutdown
// =============================================================================
bool RTPipeline::Init(ID3D12Device5* device)
{
    if (!CreateGlobalRootSignature(device)) return false;
    if (!CreateStateObject(device))         return false;

    core::Log::Info("RTPipeline initialized");
    return true;
}

void RTPipeline::Shutdown()
{
    m_shaderTable.Reset();
    m_shaderTableInstanceCount = UINT32_MAX;
    m_stateObject.Reset();
    m_globalRootSig.Reset();
    core::Log::Info("RTPipeline shut down");
}

// =============================================================================
// Global Root Signature
// =============================================================================
// Layout:
//   [0] SRV - TLAS (t0, space0)
//   [1] UAV descriptor table - output textures (u0/u1, space0)
//   [2] CBV - per-frame constants (b0, space0)
//   [3] SRV - material StructuredBuffer (t1, space0)
//   [4] SRV - triangle normal StructuredBuffer (t2, space0)
//   [5] SRV - instance metadata StructuredBuffer (t3, space0)
//   [6] SRV - triangle UV StructuredBuffer (t4, space0)
//   [7] SRV - triangle positions (t5)
//   [8] SRV descriptor table - albedo textures (t0, space4), normal textures (t0, space5)
// =============================================================================
bool RTPipeline::CreateGlobalRootSignature(ID3D12Device5* device)
{
    D3D12_ROOT_PARAMETER rootParams[9] = {};

    // Slot 0: TLAS SRV (t0)
    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace  = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 1: Output UAV descriptor table (u0)
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors     = 2;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace      = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges   = &uavRange;
    rootParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 2: Per-frame constants CBV (b0)
    rootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 0;
    rootParams[2].Descriptor.RegisterSpace  = 0;
    rootParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 3: Material StructuredBuffer SRV (t1)
    rootParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[3].Descriptor.ShaderRegister = 1;
    rootParams[3].Descriptor.RegisterSpace  = 0;
    rootParams[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 4: Triangle normal StructuredBuffer SRV (t2)
    rootParams[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[4].Descriptor.ShaderRegister = 2;
    rootParams[4].Descriptor.RegisterSpace  = 0;
    rootParams[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 5: Instance metadata StructuredBuffer SRV (t3)
    rootParams[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[5].Descriptor.ShaderRegister = 3;
    rootParams[5].Descriptor.RegisterSpace  = 0;
    rootParams[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 6: Triangle UV StructuredBuffer SRV (t4)
    rootParams[6].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[6].Descriptor.ShaderRegister = 4;
    rootParams[6].Descriptor.RegisterSpace  = 0;
    rootParams[6].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 7: Triangle position StructuredBuffer SRV (t5)
    rootParams[7].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[7].Descriptor.ShaderRegister = 5;
    rootParams[7].Descriptor.RegisterSpace  = 0;
    rootParams[7].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // Slot 8: Material texture descriptor table (albedo t0 space4, normal t0 space5)
    D3D12_DESCRIPTOR_RANGE textureRanges[2] = {};
    textureRanges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRanges[0].NumDescriptors     = kMaxRTAlbedoTextures;
    textureRanges[0].BaseShaderRegister = 0;
    textureRanges[0].RegisterSpace      = 4;
    textureRanges[0].OffsetInDescriptorsFromTableStart = 0;

    textureRanges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRanges[1].NumDescriptors     = kMaxRTNormalTextures;
    textureRanges[1].BaseShaderRegister = 0;
    textureRanges[1].RegisterSpace      = 5;
    textureRanges[1].OffsetInDescriptorsFromTableStart = kMaxRTAlbedoTextures;

    rootParams[8].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[8].DescriptorTable.NumDescriptorRanges = _countof(textureRanges);
    rootParams[8].DescriptorTable.pDescriptorRanges   = textureRanges;
    rootParams[8].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias       = 0.0f;
    sampler.MaxAnisotropy    = 1;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD           = 0.0f;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Root signature description
    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = _countof(rootParams);
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &sampler;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE; // No IA for RT

    ComPtr<ID3DBlob> sigBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &sigBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob)
            core::Log::Errorf("RT root sig error: %s",
                              static_cast<const char*>(errorBlob->GetBufferPointer()));
        return false;
    }

    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                      sigBlob->GetBufferSize(),
                                      IID_PPV_ARGS(&m_globalRootSig));
    if (FAILED(hr))
    {
        core::Log::Errorf("CreateRootSignature (RT global) failed: 0x%08X", hr);
        return false;
    }

    m_globalRootSig->SetName(L"RT_GlobalRootSig");
    core::Log::Info("RT global root signature created (9 params: TLAS, UAVs, CB, materials, normals, instances, UVs, positions, material textures)");
    return true;
}

// =============================================================================
// State Object — DXIL libraries, hit groups, shader config, pipeline config
// =============================================================================
bool RTPipeline::CreateStateObject(ID3D12Device5* device)
{
    // Compile the path tracing shader library
    auto shaderBlob = CompileShaderFromFile(
        L"shaders/path_trace.hlsl", "", "lib_6_3");

    if (!shaderBlob)
    {
        core::Log::Error("Failed to compile path tracing shaders");
        return false;
    }

    // We need to build the state object from subobjects
    // Subobjects: DXIL library, hit groups (primary + shadow),
    //             shader config, pipeline config, global root sig
    const uint32_t kSubobjectCount = 8;
    std::vector<D3D12_STATE_SUBOBJECT> subobjects(kSubobjectCount);
    uint32_t idx = 0;

    // --- 1. DXIL Library ---
    D3D12_DXIL_LIBRARY_DESC libDesc = {};
    libDesc.DXILLibrary.pShaderBytecode = shaderBlob->GetBufferPointer();
    libDesc.DXILLibrary.BytecodeLength  = shaderBlob->GetBufferSize();
    libDesc.NumExports = 0; // Export everything

    subobjects[idx].Type  = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[idx].pDesc = &libDesc;
    idx++;

    // --- 2. Hit Group: Primary (closest hit) ---
    D3D12_HIT_GROUP_DESC primaryHitGroup = {};
    primaryHitGroup.HitGroupExport           = L"PrimaryHitGroup";
    primaryHitGroup.Type                     = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    primaryHitGroup.ClosestHitShaderImport   = L"ClosestHit";
    primaryHitGroup.AnyHitShaderImport       = nullptr;
    primaryHitGroup.IntersectionShaderImport = nullptr;

    subobjects[idx].Type  = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[idx].pDesc = &primaryHitGroup;
    idx++;

    // --- 3. Hit Group: Shadow (any-hit for alpha, or empty for opaque) ---
    D3D12_HIT_GROUP_DESC shadowHitGroup = {};
    shadowHitGroup.HitGroupExport           = L"ShadowHitGroup";
    shadowHitGroup.Type                     = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    shadowHitGroup.ClosestHitShaderImport   = L"ShadowClosestHit";
    shadowHitGroup.AnyHitShaderImport       = nullptr;
    shadowHitGroup.IntersectionShaderImport = nullptr;

    subobjects[idx].Type  = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[idx].pDesc = &shadowHitGroup;
    idx++;

    // --- 4. Shader Config (payload + attribute size) ---
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    shaderConfig.MaxPayloadSizeInBytes   = 48; // hit distance, normal, UV, instance ID
    shaderConfig.MaxAttributeSizeInBytes = 8;  // float2 barycentrics (built-in triangles)

    subobjects[idx].Type  = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[idx].pDesc = &shaderConfig;
    idx++;

    // --- 5. Pipeline Config ---
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 2; // Primary + shadow bounce

    subobjects[idx].Type  = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[idx].pDesc = &pipelineConfig;
    idx++;

    // --- 6. Global Root Signature ---
    subobjects[idx].Type  = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[idx].pDesc = m_globalRootSig.GetAddressOf();
    idx++;

    // Note: We skip the remaining reserved subobjects
    // The actual count used:
    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = idx;
    stateObjectDesc.pSubobjects   = subobjects.data();

    HRESULT hr = device->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_stateObject));
    if (FAILED(hr))
    {
        core::Log::Errorf("CreateStateObject failed: 0x%08X", hr);
        return false;
    }

    // --- Retrieve shader identifiers ---
    ComPtr<ID3D12StateObjectProperties> props;
    hr = m_stateObject.As(&props);
    if (FAILED(hr) || !props)
    {
        core::Log::Errorf("Failed to query ID3D12StateObjectProperties: 0x%08X", hr);
        return false;
    }

    auto copyID = [&](const wchar_t* exportName, uint8_t* dest) {
        void* id = props->GetShaderIdentifier(exportName);
        if (!id) {
            core::Log::Errorf("GetShaderIdentifier failed for: %ls", exportName);
            return false;
        }
        memcpy(dest, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        return true;
    };

    if (!copyID(L"RayGen", m_rayGenID))           return false;
    if (!copyID(L"Miss", m_missID))                return false;
    if (!copyID(L"ShadowMiss", m_shadowMissID))    return false;
    if (!copyID(L"PrimaryHitGroup", m_hitGroupID)) return false;
    if (!copyID(L"ShadowHitGroup", m_shadowHitGroupID)) return false;

    core::Log::Info("DXR state object created (RayGen + PrimaryHit + ShadowHit + 2 Miss)");
    return true;
}

// =============================================================================
// Shader Table
// =============================================================================
// Layout (all 64-byte aligned starts, 32-byte aligned strides):
//   [Ray Gen]       1 entry: shader ID only (32 bytes, padded to 64)
//   [Miss]          2 entries: primary miss + shadow miss
//   [Hit Group]     N×2 entries: (primary + shadow) per instance
// =============================================================================
bool RTPipeline::BuildShaderTable(ID3D12Device5* device, uint32_t instanceCount)
{
    if (m_shaderTable && m_shaderTableInstanceCount == instanceCount)
        return true;

    const uint64_t idSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
    const uint64_t recordAlign = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT; // 32
    const uint64_t tableAlign  = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;  // 64

    // Entry sizes (shader ID + local root args, padded to 32-byte alignment)
    // No local root arguments for any shader — using global bindless approach
    m_rayGenEntrySize   = Align(idSize, recordAlign); // 32
    m_missEntrySize     = Align(idSize, recordAlign);  // 32
    m_hitGroupEntrySize = Align(idSize, recordAlign);  // 32

    m_missCount     = 2; // Primary miss + shadow miss
    m_hitGroupCount = instanceCount * 2; // Primary + shadow per instance

    // Calculate section offsets (64-byte aligned)
    m_rayGenOffset   = 0;
    uint64_t rayGenSectionSize = Align(m_rayGenEntrySize, tableAlign);

    m_missOffset = Align(m_rayGenOffset + rayGenSectionSize, tableAlign);
    uint64_t missSectionSize = Align(m_missEntrySize * m_missCount, tableAlign);

    m_hitGroupOffset = Align(m_missOffset + missSectionSize, tableAlign);
    uint64_t hitGroupSectionSize = Align(m_hitGroupEntrySize * m_hitGroupCount, tableAlign);

    uint64_t totalSize = m_hitGroupOffset + hitGroupSectionSize;

    // Create upload buffer for shader table
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = totalSize;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_shaderTable));

    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create shader table: 0x%08X", hr);
        return false;
    }
    m_shaderTable->SetName(L"RT_ShaderTable");
    m_shaderTableSize = totalSize;

    // Map and fill
    uint8_t* mapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = m_shaderTable->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr) || !mapped)
    {
        core::Log::Errorf("Failed to map shader table: 0x%08X", hr);
        m_shaderTable.Reset();
        m_shaderTableSize = 0;
        return false;
    }
    memset(mapped, 0, static_cast<size_t>(totalSize));

    // Ray generation record
    memcpy(mapped + m_rayGenOffset, m_rayGenID, idSize);

    // Miss records
    uint8_t* missBase = mapped + m_missOffset;
    memcpy(missBase + 0 * m_missEntrySize, m_missID, idSize);        // Primary miss
    memcpy(missBase + 1 * m_missEntrySize, m_shadowMissID, idSize);  // Shadow miss

    // Hit group records: for each instance, primary + shadow
    uint8_t* hitBase = mapped + m_hitGroupOffset;
    for (uint32_t i = 0; i < instanceCount; i++)
    {
        uint64_t offset = static_cast<uint64_t>(i) * 2 * m_hitGroupEntrySize;
        memcpy(hitBase + offset + 0 * m_hitGroupEntrySize, m_hitGroupID, idSize);
        memcpy(hitBase + offset + 1 * m_hitGroupEntrySize, m_shadowHitGroupID, idSize);
    }

    m_shaderTable->Unmap(0, nullptr);
    m_shaderTableInstanceCount = instanceCount;

    core::Log::Infof("Shader table built: %llu bytes (%u instances, %u hit groups)",
                     static_cast<unsigned long long>(totalSize),
                     instanceCount, m_hitGroupCount);
    return true;
}

// =============================================================================
// SBT Accessors
// =============================================================================
D3D12_GPU_VIRTUAL_ADDRESS RTPipeline::RayGenAddress() const
{
    return m_shaderTable->GetGPUVirtualAddress() + m_rayGenOffset;
}

uint64_t RTPipeline::RayGenSize() const { return m_rayGenEntrySize; }

D3D12_GPU_VIRTUAL_ADDRESS RTPipeline::MissAddress() const
{
    return m_shaderTable->GetGPUVirtualAddress() + m_missOffset;
}

uint64_t RTPipeline::MissSize() const   { return m_missEntrySize * m_missCount; }
uint64_t RTPipeline::MissStride() const { return m_missEntrySize; }

D3D12_GPU_VIRTUAL_ADDRESS RTPipeline::HitGroupAddress() const
{
    return m_shaderTable->GetGPUVirtualAddress() + m_hitGroupOffset;
}

uint64_t RTPipeline::HitGroupSize() const   { return m_hitGroupEntrySize * m_hitGroupCount; }
uint64_t RTPipeline::HitGroupStride() const { return m_hitGroupEntrySize; }

} // namespace render
