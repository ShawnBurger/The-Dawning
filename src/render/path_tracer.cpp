// =============================================================================
// render/path_tracer.cpp — Path Tracer Implementation
// =============================================================================

#include "path_tracer.h"
#include "../core/log.h"
#include <cstring>

namespace render
{

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

    m_pipeline.Shutdown();
    m_accel.Shutdown();

    m_outputTexture.Reset();
    m_srvUavHeap.Reset();
    m_materialBuffer.Reset();
    for (auto& cb : m_constantBuffer) cb.Reset();

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

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc       = { 1, 0 };
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(&m_outputTexture));

    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create RT output texture: 0x%08X", hr);
        return false;
    }
    m_outputTexture->SetName(L"RT_OutputTexture");

    // Create UAV in descriptor heap slot 0
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format               = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.ViewDimension        = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice   = 0;

    device->CreateUnorderedAccessView(
        m_outputTexture.Get(), nullptr, &uavDesc,
        m_srvUavHeap->GetCPUDescriptorHandleForHeapStart());

    core::Log::Infof("RT output texture created: %ux%u", width, height);
    return true;
}

void PathTracer::Resize(ID3D12Device5* device, uint32_t width, uint32_t height)
{
    if (width == m_outputWidth && height == m_outputHeight) return;
    m_outputTexture.Reset();
    CreateOutputTexture(device, width, height);
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
void PathTracer::Dispatch(
    D3D12Device& device,
    const Camera& camera,
    const core::Vec3f& lightDir,
    const core::Vec3f& lightColor,
    const core::Vec3f& ambientColor,
    const RTMaterialData* materials,
    uint32_t materialCount,
    uint32_t instanceCount)
{
    if (!m_initialized) return;
    (void)instanceCount;

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
    cb.cameraPos[0] = camPos.x; cb.cameraPos[1] = camPos.y; cb.cameraPos[2] = camPos.z;
    cb.cameraRight[0] = camRight.x; cb.cameraRight[1] = camRight.y; cb.cameraRight[2] = camRight.z;
    cb.cameraUp[0] = camUp.x; cb.cameraUp[1] = camUp.y; cb.cameraUp[2] = camUp.z;
    cb.cameraForward[0] = camForward.x; cb.cameraForward[1] = camForward.y; cb.cameraForward[2] = camForward.z;
    cb.lightDir[0] = lightDir.x; cb.lightDir[1] = lightDir.y; cb.lightDir[2] = lightDir.z;
    cb.lightColor[0] = lightColor.x; cb.lightColor[1] = lightColor.y; cb.lightColor[2] = lightColor.z;
    cb.ambientColor[0] = ambientColor.x; cb.ambientColor[1] = ambientColor.y; cb.ambientColor[2] = ambientColor.z;

    static uint32_t s_globalFrame = 0;
    cb.frameIndex    = s_globalFrame++;
    cb.maxBounces    = 3;
    cb.renderWidth   = m_outputWidth;
    cb.renderHeight  = m_outputHeight;

    memcpy(m_cbMapped[m_frameIndex], &cb, sizeof(cb));

    // --- Upload materials ---
    uint32_t matCount = materialCount < m_maxMaterials ? materialCount : m_maxMaterials;
    if (matCount > 0 && materials)
        memcpy(m_materialMapped, materials, sizeof(RTMaterialData) * matCount);

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

    // UAV barrier before reading the output
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_outputTexture.Get();
    cmd->ResourceBarrier(1, &barrier);
}

// =============================================================================
// Copy RT output to back buffer
// =============================================================================
void PathTracer::CopyToBackBuffer(D3D12Device& device)
{
    if (!m_outputTexture) return;

    auto* cmd = device.CmdList();

    // Transition output: UAV → COPY_SOURCE
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource   = m_outputTexture.Get();
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
    cmd->CopyResource(device.CurrentBackBuffer(), m_outputTexture.Get());

    // Transition output back: COPY_SOURCE → UAV
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Transition back buffer: COPY_DEST → PRESENT
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;

    cmd->ResourceBarrier(2, barriers);
}

} // namespace render
