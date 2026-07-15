// =============================================================================
// render/path_tracer.cpp — Path Tracer Implementation
// =============================================================================

#include "path_tracer.h"
#include "../core/log.h"
#include <cstring>

namespace render
{

static bool CreateMappedUploadBuffer(
    ID3D12Device5* device,
    uint64_t byteSize,
    const wchar_t* name,
    ComPtr<ID3D12Resource>& buffer,
    uint8_t** mapped)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = byteSize;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&buffer));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create RT upload buffer '%ls': 0x%08X", name, hr);
        return false;
    }

    buffer->SetName(name);
    D3D12_RANGE readRange = { 0, 0 };
    hr = buffer->Map(0, &readRange, reinterpret_cast<void**>(mapped));
    if (FAILED(hr) || !*mapped)
    {
        core::Log::Errorf("Failed to map RT upload buffer '%ls': 0x%08X", name, hr);
        buffer.Reset();
        return false;
    }

    return true;
}

static bool HasVectorChanged(const core::Vec3f& a, const core::Vec3f& b, float epsilon = 1e-5f)
{
    return (a - b).LengthSq() > epsilon * epsilon;
}

// =============================================================================
// Init / Shutdown
// =============================================================================
bool PathTracer::Init(D3D12Device& device)
{
    auto* dev5 = device.Device5();
    if (!dev5 || !device.Caps().raytracing)
    {
        core::Log::Error("PathTracer requires a DXR-capable ID3D12Device5");
        return false;
    }

    if (!m_accel.Init(dev5))    return false;
    if (!m_pipeline.Init(dev5)) return false;

    if (!CreateDescriptorHeap(dev5)) return false;
    if (!CreateOutputTexture(dev5, device.Width(), device.Height())) return false;
    if (!CreateConstantBuffer(dev5)) return false;
    if (!CreateMaterialBuffer(dev5, 256)) return false;

    m_initialized = true;
    core::Log::Info("PathTracer initialized");
    return true;
}

void PathTracer::Shutdown()
{
    for (int i = 0; i < 3; i++)
    {
        if (m_constantBuffer[i] && m_cbMapped[i])
        {
            m_constantBuffer[i]->Unmap(0, nullptr);
            m_cbMapped[i] = nullptr;
        }
    }

    if (m_materialBuffer && m_materialMapped)
    {
        m_materialBuffer->Unmap(0, nullptr);
        m_materialMapped = nullptr;
    }

    if (m_instanceDataBuffer && m_instanceDataMapped)
    {
        m_instanceDataBuffer->Unmap(0, nullptr);
        m_instanceDataMapped = nullptr;
    }

    if (m_triangleNormalBuffer && m_triangleNormalMapped)
    {
        m_triangleNormalBuffer->Unmap(0, nullptr);
        m_triangleNormalMapped = nullptr;
    }

    m_pipeline.Shutdown();
    m_accel.Shutdown();

    m_outputTexture.Reset();
    m_displayTexture.Reset();
    m_srvUavHeap.Reset();
    m_materialBuffer.Reset();
    m_instanceDataBuffer.Reset();
    m_triangleNormalBuffer.Reset();
    for (auto& cb : m_constantBuffer) cb.Reset();

    m_maxInstanceData = 0;
    m_maxTriangleNormals = 0;
    m_accumFrameIndex = 0;
    m_hasPrevCamera = false;
    m_initialized = false;
    core::Log::Info("PathTracer shut down");
}

// =============================================================================
// Create SRV/UAV Descriptor Heap
// =============================================================================
bool PathTracer::CreateDescriptorHeap(ID3D12Device5* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 4; // UAV output + future SRVs
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvUavHeap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create RT descriptor heap: 0x%08X", hr);
        return false;
    }
    m_srvUavHeap->SetName(L"RT_SrvUavHeap");
    return true;
}

// =============================================================================
// Create/Resize Output Texture
// =============================================================================
bool PathTracer::CreateOutputTexture(ID3D12Device5* device, uint32_t width, uint32_t height)
{
    m_outputWidth  = width;
    m_outputHeight = height;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.SampleDesc       = { 1, 0 };
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(&m_outputTexture));

    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create RT output texture: 0x%08X", hr);
        return false;
    }
    m_outputTexture->SetName(L"RT_HDRHistoryTexture");

    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(&m_displayTexture));

    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create RT display texture: 0x%08X", hr);
        return false;
    }
    m_displayTexture->SetName(L"RT_DisplayTexture");

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension        = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice   = 0;

    // UAV slot 0: HDR linear radiance history
    uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(
        m_outputTexture.Get(), nullptr, &uavDesc,
        m_srvUavHeap->GetCPUDescriptorHandleForHeapStart());

    // UAV slot 1: tone-mapped 8-bit display output
    UINT descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE displayHandle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    displayHandle.ptr += descriptorSize;
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateUnorderedAccessView(
        m_displayTexture.Get(), nullptr, &uavDesc, displayHandle);

    core::Log::Infof("RT output textures created: %ux%u (HDR history + display)", width, height);
    return true;
}

void PathTracer::Resize(ID3D12Device5* device, uint32_t width, uint32_t height)
{
    if (width == m_outputWidth && height == m_outputHeight) return;
    m_outputTexture.Reset();
    m_displayTexture.Reset();
    CreateOutputTexture(device, width, height);
    m_accumFrameIndex = 0;
    m_hasPrevCamera = false;
}

// =============================================================================
// Per-frame Constant Buffer (one per frame in flight)
// =============================================================================
bool PathTracer::CreateConstantBuffer(ID3D12Device5* device)
{
    const uint32_t cbSize = (sizeof(RTPerFrameConstants) + 255) & ~255;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = cbSize;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const wchar_t* names[] = { L"RT_CB[0]", L"RT_CB[1]", L"RT_CB[2]" };

    for (int i = 0; i < 3; i++)
    {
        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_constantBuffer[i]));
        if (FAILED(hr)) return false;

        m_constantBuffer[i]->SetName(names[i]);
        D3D12_RANGE readRange = { 0, 0 };
        m_constantBuffer[i]->Map(0, &readRange,
                                  reinterpret_cast<void**>(&m_cbMapped[i]));
    }

    return true;
}

// =============================================================================
// Material StructuredBuffer (upload heap, updated per frame)
// =============================================================================
bool PathTracer::CreateMaterialBuffer(ID3D12Device5* device, uint32_t maxMaterials)
{
    m_maxMaterials = maxMaterials;
    uint64_t bufSize = sizeof(RTMaterialData) * maxMaterials;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = bufSize;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_materialBuffer));
    if (FAILED(hr)) return false;

    m_materialBuffer->SetName(L"RT_MaterialBuffer");
    D3D12_RANGE readRange = { 0, 0 };
    m_materialBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_materialMapped));

    return true;
}

// =============================================================================
// Dispatch — execute the path tracer for one frame
// =============================================================================
bool PathTracer::EnsureInstanceDataBuffer(ID3D12Device5* device, uint32_t instanceCount)
{
    if (instanceCount == 0) return false;
    if (m_instanceDataBuffer && instanceCount <= m_maxInstanceData) return true;

    if (m_instanceDataBuffer && m_instanceDataMapped)
    {
        m_instanceDataBuffer->Unmap(0, nullptr);
        m_instanceDataMapped = nullptr;
    }
    m_instanceDataBuffer.Reset();

    m_maxInstanceData = instanceCount + 64;
    uint64_t bufSize = sizeof(RTInstanceData) * static_cast<uint64_t>(m_maxInstanceData);
    return CreateMappedUploadBuffer(device, bufSize, L"RT_InstanceDataBuffer",
                                    m_instanceDataBuffer, &m_instanceDataMapped);
}

bool PathTracer::EnsureTriangleNormalBuffer(ID3D12Device5* device, uint32_t triangleCount)
{
    if (triangleCount == 0) return false;
    if (m_triangleNormalBuffer && triangleCount <= m_maxTriangleNormals) return true;

    if (m_triangleNormalBuffer && m_triangleNormalMapped)
    {
        m_triangleNormalBuffer->Unmap(0, nullptr);
        m_triangleNormalMapped = nullptr;
    }
    m_triangleNormalBuffer.Reset();

    m_maxTriangleNormals = triangleCount + 256;
    uint64_t bufSize = sizeof(RTTriangleNormalData) * static_cast<uint64_t>(m_maxTriangleNormals);
    return CreateMappedUploadBuffer(device, bufSize, L"RT_TriangleNormalBuffer",
                                    m_triangleNormalBuffer, &m_triangleNormalMapped);
}

void PathTracer::Dispatch(
    D3D12Device& device,
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
    uint32_t instanceCount)
{
    if (!m_initialized) return;

    if (!m_accel.GetTLASAddress())
    {
        core::Log::Error("PathTracer dispatch skipped: TLAS is not built");
        return;
    }

    if (!m_pipeline.HasShaderTable())
    {
        core::Log::Error("PathTracer dispatch skipped: shader table is not built");
        return;
    }

    if (materialCount != instanceCount || instanceDataCount != instanceCount)
    {
        core::Log::Errorf("PathTracer dispatch skipped: instance data mismatch (instances=%u materials=%u metadata=%u)",
                          instanceCount, materialCount, instanceDataCount);
        return;
    }

    if (!materials || !instanceData || !triangleNormals || triangleNormalCount == 0)
    {
        core::Log::Error("PathTracer dispatch skipped: missing RT scene metadata");
        return;
    }

    m_frameIndex = device.FrameIndex();

    auto* cmd = device.CmdList4();
    if (!cmd)
    {
        core::Log::Error("PathTracer dispatch requires ID3D12GraphicsCommandList4");
        return;
    }

    // --- Upload per-frame constants ---
    float aspect = static_cast<float>(m_outputWidth) / static_cast<float>(m_outputHeight);
    core::Mat4x4 view = camera.ViewMatrix();
    core::Mat4x4 proj = camera.ProjectionMatrix(aspect);
    core::Mat4x4 viewProj = view * proj;

    // We need the inverse view-projection for ray generation
    // For now, compute a simple inverse (our Mat4x4 may not have a general inverse yet)
    // The ray gen shader will reconstruct ray directions from screen coordinates
    RTPerFrameConstants cb = {};
    memcpy(cb.viewProj, viewProj.Data(), sizeof(float) * 16);
    // Note: this is actually viewProj, not the inverse — the shader will handle reconstruction
    // via camera position + screen-space ray direction calculation

    core::Vec3f camPos = camera.Position();
    core::Vec3f camRight = camera.Right();
    core::Vec3f camForward = camera.Forward();
    core::Vec3f camUp = camForward.Cross(camRight).Normalized();

    bool cameraChanged = !m_hasPrevCamera ||
        HasVectorChanged(camPos, m_prevCameraPos) ||
        HasVectorChanged(camRight, m_prevCameraRight) ||
        HasVectorChanged(camUp, m_prevCameraUp) ||
        HasVectorChanged(camForward, m_prevCameraForward);

    if (cameraChanged)
    {
        m_accumFrameIndex = 0;
    }
    else if (m_accumFrameIndex < UINT32_MAX)
    {
        m_accumFrameIndex++;
    }

    m_prevCameraPos = camPos;
    m_prevCameraRight = camRight;
    m_prevCameraUp = camUp;
    m_prevCameraForward = camForward;
    m_hasPrevCamera = true;

    cb.cameraPos[0] = camPos.x; cb.cameraPos[1] = camPos.y; cb.cameraPos[2] = camPos.z;
    cb.cameraRight[0] = camRight.x; cb.cameraRight[1] = camRight.y; cb.cameraRight[2] = camRight.z;
    cb.cameraUp[0] = camUp.x; cb.cameraUp[1] = camUp.y; cb.cameraUp[2] = camUp.z;
    cb.cameraForward[0] = camForward.x; cb.cameraForward[1] = camForward.y; cb.cameraForward[2] = camForward.z;
    cb.lightDir[0] = lightDir.x; cb.lightDir[1] = lightDir.y; cb.lightDir[2] = lightDir.z;
    cb.lightColor[0] = lightColor.x; cb.lightColor[1] = lightColor.y; cb.lightColor[2] = lightColor.z;
    cb.ambientColor[0] = ambientColor.x; cb.ambientColor[1] = ambientColor.y; cb.ambientColor[2] = ambientColor.z;

    cb.frameIndex    = m_accumFrameIndex;
    cb.maxBounces    = 1;
    cb.renderWidth   = m_outputWidth;
    cb.renderHeight  = m_outputHeight;

    memcpy(m_cbMapped[m_frameIndex], &cb, sizeof(cb));

    // --- Upload materials ---
    uint32_t matCount = materialCount < m_maxMaterials ? materialCount : m_maxMaterials;
    if (matCount > 0 && materials)
        memcpy(m_materialMapped, materials, sizeof(RTMaterialData) * matCount);

    if (!EnsureInstanceDataBuffer(device.Device5(), instanceDataCount) ||
        !EnsureTriangleNormalBuffer(device.Device5(), triangleNormalCount))
    {
        core::Log::Error("PathTracer dispatch skipped: failed to prepare RT geometry buffers");
        return;
    }

    memcpy(m_instanceDataMapped, instanceData, sizeof(RTInstanceData) * instanceDataCount);
    memcpy(m_triangleNormalMapped, triangleNormals, sizeof(RTTriangleNormalData) * triangleNormalCount);

    // --- Set up for DispatchRays ---
    cmd->SetComputeRootSignature(m_pipeline.GetGlobalRootSig());
    cmd->SetPipelineState1(m_pipeline.GetStateObject());

    // Set descriptor heap
    ID3D12DescriptorHeap* heaps[] = { m_srvUavHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    // Bind global root parameters
    // [0] TLAS SRV
    cmd->SetComputeRootShaderResourceView(0, m_accel.GetTLASAddress());
    // [1] Output UAV descriptor table
    cmd->SetComputeRootDescriptorTable(1,
        m_srvUavHeap->GetGPUDescriptorHandleForHeapStart());
    // [2] Per-frame CB
    cmd->SetComputeRootConstantBufferView(2,
        m_constantBuffer[m_frameIndex]->GetGPUVirtualAddress());
    // [3] Material buffer
    cmd->SetComputeRootShaderResourceView(3,
        m_materialBuffer->GetGPUVirtualAddress());
    // [4] Triangle normal buffer
    cmd->SetComputeRootShaderResourceView(4,
        m_triangleNormalBuffer->GetGPUVirtualAddress());
    // [5] Instance metadata buffer
    cmd->SetComputeRootShaderResourceView(5,
        m_instanceDataBuffer->GetGPUVirtualAddress());

    // --- DispatchRays ---
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};

    dispatchDesc.RayGenerationShaderRecord.StartAddress = m_pipeline.RayGenAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes  = m_pipeline.RayGenSize();

    dispatchDesc.MissShaderTable.StartAddress  = m_pipeline.MissAddress();
    dispatchDesc.MissShaderTable.SizeInBytes   = m_pipeline.MissSize();
    dispatchDesc.MissShaderTable.StrideInBytes = m_pipeline.MissStride();

    dispatchDesc.HitGroupTable.StartAddress  = m_pipeline.HitGroupAddress();
    dispatchDesc.HitGroupTable.SizeInBytes   = m_pipeline.HitGroupSize();
    dispatchDesc.HitGroupTable.StrideInBytes = m_pipeline.HitGroupStride();

    dispatchDesc.Width  = m_outputWidth;
    dispatchDesc.Height = m_outputHeight;
    dispatchDesc.Depth  = 1;

    cmd->DispatchRays(&dispatchDesc);

    // UAV barriers before copying the tone-mapped display output.
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_outputTexture.Get();
    barriers[1].Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_displayTexture.Get();
    cmd->ResourceBarrier(2, barriers);
}

// =============================================================================
// Copy RT output to back buffer
// =============================================================================
void PathTracer::CopyToBackBuffer(D3D12Device& device)
{
    if (!m_displayTexture) return;

    auto* cmd = device.CmdList();

    // Transition output: UAV → COPY_SOURCE
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource   = m_displayTexture.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Transition back buffer: RENDER_TARGET → COPY_DEST
    barriers[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource   = device.CurrentBackBuffer();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmd->ResourceBarrier(2, barriers);

    // Copy
    cmd->CopyResource(device.CurrentBackBuffer(), m_displayTexture.Get());

    // Transition output back: COPY_SOURCE → UAV
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Transition back buffer: COPY_DEST → PRESENT
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;

    cmd->ResourceBarrier(2, barriers);
}

} // namespace render
