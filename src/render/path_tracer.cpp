// =============================================================================
// render/path_tracer.cpp — Path Tracer Implementation
// =============================================================================

#include "path_tracer.h"
#include "../core/log.h"
#include <algorithm>
#include <cstring>
#include <cstdint>

namespace render
{

static constexpr uint32_t kRTUavDescriptorCount = 2;
static constexpr uint32_t kRTAlbedoDescriptorBase = kRTUavDescriptorCount;
static constexpr uint32_t kRTNormalDescriptorBase = kRTAlbedoDescriptorBase + kMaxRTAlbedoTextures;

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

static bool HasPositionChanged(const core::Vec3d& a,
                               const core::Vec3d& b,
                               double epsilon = 1e-5)
{
    return (a - b).LengthSq() > epsilon * epsilon;
}

RTQualityInfo GetRTQualityInfo(RTQualityMode mode)
{
    if (mode == RTQualityMode::FullPathTrace)
        return { "Full Path Trace", "FULL", 4, 3, 0 };
    return { "Stable Preview", "STABLE", 8, 1, 1 };
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

    m_pipeline.Shutdown();
    m_accel.Shutdown();

    m_outputTexture.Reset();
    m_displayTexture.Reset();
    m_srvUavHeap.Reset();

    // FrameUploadBuffer::Reset unmaps and releases every frame instance.
    m_materialBuffers.Reset();
    m_instanceDataBuffers.Reset();
    m_triangleNormalBuffers.Reset();
    m_triangleUVBuffers.Reset();
    m_trianglePositionBuffers.Reset();
    for (auto& cb : m_constantBuffer) cb.Reset();

    m_srvUavDescSize = 0;
    m_boundAlbedoTextureCount = 0;
    m_boundAlbedoTextureResources.fill(nullptr);
    m_boundNormalTextureCount = 0;
    m_boundNormalTextureResources.fill(nullptr);
    m_accumFrameIndex = 0;
    m_hasPrevCamera = false;
    m_hasPrevQuality = false;
    m_initialized = false;
    core::Log::Info("PathTracer shut down");
}

// =============================================================================
// Create SRV/UAV Descriptor Heap
// =============================================================================
bool PathTracer::CreateDescriptorHeap(ID3D12Device5* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = kRTNormalDescriptorBase + kMaxRTNormalTextures;
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvUavHeap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create RT descriptor heap: 0x%08X", hr);
        return false;
    }
    m_srvUavHeap->SetName(L"RT_SrvUavHeap");
    m_srvUavDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ClearMaterialTextureDescriptors(device);
    return true;
}

void PathTracer::ClearMaterialTextureDescriptors(ID3D12Device5* device)
{
    if (!device || !m_srvUavHeap || m_srvUavDescSize == 0)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;

    auto clearRange = [&](uint32_t firstDescriptor, uint32_t descriptorCount)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(firstDescriptor) * m_srvUavDescSize;
        for (uint32_t i = 0; i < descriptorCount; ++i)
        {
            device->CreateShaderResourceView(nullptr, &nullSrv, handle);
            handle.ptr += m_srvUavDescSize;
        }
    };

    clearRange(kRTAlbedoDescriptorBase, kMaxRTAlbedoTextures);
    clearRange(kRTNormalDescriptorBase, kMaxRTNormalTextures);

    m_boundAlbedoTextureCount = 0;
    m_boundAlbedoTextureResources.fill(nullptr);
    m_boundNormalTextureCount = 0;
    m_boundNormalTextureResources.fill(nullptr);
}

uint32_t PathTracer::UpdateTextureDescriptors(
    ID3D12Device5* device,
    const Texture* const* textures,
    uint32_t textureCount,
    uint32_t firstDescriptor,
    uint32_t maxDescriptors,
    uint32_t& boundCount,
    ID3D12Resource** boundResources)
{
    if (!device || !m_srvUavHeap || m_srvUavDescSize == 0)
        return 0;

    const uint32_t desiredCount = (std::min)(textureCount, maxDescriptors);
    bool unchanged = desiredCount == boundCount;
    for (uint32_t i = 0; i < maxDescriptors; ++i)
    {
        ID3D12Resource* desired = nullptr;
        if (i < desiredCount && textures && textures[i] && textures[i]->IsValid())
            desired = textures[i]->resource.Get();
        if (boundResources[i] != desired)
        {
            unchanged = false;
            break;
        }
    }
    if (unchanged)
        return desiredCount;

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(firstDescriptor) * m_srvUavDescSize;

    for (uint32_t i = 0; i < maxDescriptors; ++i)
    {
        device->CreateShaderResourceView(nullptr, &nullSrv, handle);
        boundResources[i] = nullptr;
        handle.ptr += m_srvUavDescSize;
    }

    handle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(firstDescriptor) * m_srvUavDescSize;

    for (uint32_t i = 0; i < desiredCount; ++i)
    {
        const Texture* texture = textures ? textures[i] : nullptr;
        if (!texture || !texture->IsValid())
        {
            handle.ptr += m_srvUavDescSize;
            continue;
        }

        boundResources[i] = texture->resource.Get();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texture->format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = texture->mipCount;
        device->CreateShaderResourceView(texture->resource.Get(), &srvDesc, handle);
        handle.ptr += m_srvUavDescSize;
    }

    boundCount = desiredCount;
    return desiredCount;
}

// =============================================================================
// Create/Resize Output Texture
// =============================================================================
bool PathTracer::CreateOutputTexture(ID3D12Device5* device, uint32_t width, uint32_t height)
{
    // Dimensions are committed at the END, on the success path only. Assigning them
    // here would make a failed create look like a completed one: Resize()'s early-out
    // would then treat every retry at these dimensions as a no-op, permanently.
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
        // Leave nothing half-built: Dispatch() guards on these being null.
        m_outputTexture.Reset();
        m_displayTexture.Reset();
        m_outputWidth = m_outputHeight = 0;
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
        m_outputTexture.Reset();
        m_displayTexture.Reset();
        m_outputWidth = m_outputHeight = 0;
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
    D3D12_CPU_DESCRIPTOR_HANDLE displayHandle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    displayHandle.ptr += m_srvUavDescSize;
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateUnorderedAccessView(
        m_displayTexture.Get(), nullptr, &uavDesc, displayHandle);

    // Both resources and both descriptors exist — only now is this size real.
    m_outputWidth  = width;
    m_outputHeight = height;

    core::Log::Infof("RT output textures created: %ux%u (HDR history + display)", width, height);
    return true;
}

bool PathTracer::Resize(ID3D12Device5* device, uint32_t width, uint32_t height)
{
    if (width == m_outputWidth && height == m_outputHeight) return true;

    m_outputTexture.Reset();
    m_displayTexture.Reset();

    const bool ok = CreateOutputTexture(device, width, height);

    m_accumFrameIndex = 0;
    m_hasPrevCamera = false;
    m_hasPrevQuality = false;

    if (!ok)
    {
        // m_outputWidth/Height are now 0, so a later resize to any size retries
        // rather than early-outing. Dispatch() skips work while the textures are null.
        core::Log::Errorf("Path tracer resize to %ux%u failed; path tracing is disabled "
                          "until a later resize succeeds", width, height);
    }
    return ok;
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
        hr = m_constantBuffer[i]->Map(0, &readRange,
                                      reinterpret_cast<void**>(&m_cbMapped[i]));
        if (FAILED(hr) || !m_cbMapped[i])
        {
            core::Log::Errorf("Failed to map RT constant buffer %d: 0x%08X", i, hr);
            m_cbMapped[i] = nullptr;
            return false;
        }
    }

    return true;
}


// =============================================================================
// Dispatch — execute the path tracer for one frame
// =============================================================================
// =============================================================================
// Per-frame RT upload buffers
// =============================================================================
// One growth path for all five. Allocates kFrameCount instances so each frame in
// flight writes its own copy; previously these were single-instanced and the
// per-frame CPU memcpy raced any command list still reading them. The full
// WaitForGpu at the end of each path-traced frame hid that, at the cost of
// pinning RT to zero frames in flight.
bool PathTracer::EnsureFrameUploadBuffer(ID3D12Device5* device,
                                         FrameUploadBuffer& target,
                                         uint32_t elementCount,
                                         uint64_t elementSize,
                                         const wchar_t* debugName)
{
    if (elementCount == 0) return false;
    if (target.Valid() && elementCount <= target.capacity) return true;

    target.Reset();

    // Same headroom the per-buffer versions used, so growth still amortises.
    const uint32_t newCapacity = elementCount + 64;
    const uint64_t byteSize = elementSize * static_cast<uint64_t>(newCapacity);

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        wchar_t name[96];
        swprintf_s(name, L"%s[%u]", debugName, i);
        if (!CreateMappedUploadBuffer(device, byteSize, name,
                                      target.buffer[i], &target.mapped[i]))
        {
            // Partial allocation is worse than none: Dispatch would write some
            // frames and not others.
            target.Reset();
            return false;
        }
    }

    target.capacity = newCapacity;
    return true;
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
    const RTTriangleUVData* triangleUVs,
    uint32_t triangleUVCount,
    const RTTrianglePositionData* trianglePositions,
    uint32_t trianglePositionCount,
    const Texture* const* albedoTextures,
    uint32_t albedoTextureCount,
    const Texture* const* normalTextures,
    uint32_t normalTextureCount,
    uint32_t instanceCount,
    RTQualityMode qualityMode)
{
    if (!m_initialized) return;

    // A failed Resize() leaves us with no output textures while m_initialized stays
    // true. Without this guard the UAV descriptors in heap slots 0 and 1 still point
    // at the released resources and DispatchRays would write through them.
    // Resize() already logged (and latched) the error, so stay silent here rather
    // than emitting one error per frame for the rest of the run.
    if (!m_outputTexture || !m_displayTexture) return;

    RTQualityInfo quality = GetRTQualityInfo(qualityMode);

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

    if (!materials || !instanceData || !triangleNormals || !triangleUVs || !trianglePositions ||
        triangleNormalCount == 0 || triangleUVCount == 0 || trianglePositionCount == 0)
    {
        core::Log::Error("PathTracer dispatch skipped: missing RT scene metadata");
        return;
    }

    if (triangleUVCount != triangleNormalCount || trianglePositionCount != triangleNormalCount)
    {
        core::Log::Errorf("PathTracer dispatch skipped: triangle metadata mismatch (normals=%u uvs=%u positions=%u)",
                          triangleNormalCount, triangleUVCount, trianglePositionCount);
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

    const core::Vec3d& cameraWorldPos = camera.Position();
    const core::Vec3f camPos = {};
    core::Vec3f camRight = camera.Right();
    core::Vec3f camForward = camera.Forward();
    core::Vec3f camUp = camForward.Cross(camRight).Normalized();

    bool cameraChanged = !m_hasPrevCamera ||
        HasPositionChanged(cameraWorldPos, m_prevCameraPos) ||
        HasVectorChanged(camRight, m_prevCameraRight) ||
        HasVectorChanged(camUp, m_prevCameraUp) ||
        HasVectorChanged(camForward, m_prevCameraForward);

    bool qualityChanged = !m_hasPrevQuality ||
        qualityMode != m_prevQualityMode ||
        quality.samplesPerPixel != m_prevSamplesPerPixel ||
        quality.maxBounces != m_prevMaxBounces;

    if (cameraChanged || qualityChanged)
    {
        m_accumFrameIndex = 0;
    }
    else if (m_accumFrameIndex < UINT32_MAX)
    {
        m_accumFrameIndex++;
    }

    m_prevCameraPos = cameraWorldPos;
    m_prevCameraRight = camRight;
    m_prevCameraUp = camUp;
    m_prevCameraForward = camForward;
    m_hasPrevCamera = true;
    m_prevQualityMode = qualityMode;
    m_prevSamplesPerPixel = quality.samplesPerPixel;
    m_prevMaxBounces = quality.maxBounces;
    m_hasPrevQuality = true;

    cb.cameraPos[0] = camPos.x; cb.cameraPos[1] = camPos.y; cb.cameraPos[2] = camPos.z;
    cb.cameraRight[0] = camRight.x; cb.cameraRight[1] = camRight.y; cb.cameraRight[2] = camRight.z;
    cb.cameraUp[0] = camUp.x; cb.cameraUp[1] = camUp.y; cb.cameraUp[2] = camUp.z;
    cb.cameraForward[0] = camForward.x; cb.cameraForward[1] = camForward.y; cb.cameraForward[2] = camForward.z;
    cb.lightDir[0] = lightDir.x; cb.lightDir[1] = lightDir.y; cb.lightDir[2] = lightDir.z;
    cb.lightColor[0] = lightColor.x; cb.lightColor[1] = lightColor.y; cb.lightColor[2] = lightColor.z;
    cb.ambientColor[0] = ambientColor.x; cb.ambientColor[1] = ambientColor.y; cb.ambientColor[2] = ambientColor.z;

    cb.frameIndex    = m_accumFrameIndex;
    cb.seedIndex     = m_seedFrameCounter++;
    cb.maxBounces    = quality.maxBounces;
    cb.renderWidth   = m_outputWidth;
    cb.renderHeight  = m_outputHeight;
    cb.samplesPerPixel = quality.samplesPerPixel;
    cb.stablePreview = quality.stablePreview;

    memcpy(m_cbMapped[m_frameIndex], &cb, sizeof(cb));

    auto* dev5 = device.Device5();
    if (!EnsureFrameUploadBuffer(dev5, m_materialBuffers, materialCount,
                                 sizeof(RTMaterialData), L"RT_MaterialBuffer") ||
        !EnsureFrameUploadBuffer(dev5, m_instanceDataBuffers, instanceDataCount,
                                 sizeof(RTInstanceData), L"RT_InstanceDataBuffer") ||
        !EnsureFrameUploadBuffer(dev5, m_triangleNormalBuffers, triangleNormalCount,
                                 sizeof(RTTriangleNormalData), L"RT_TriangleNormalBuffer") ||
        !EnsureFrameUploadBuffer(dev5, m_triangleUVBuffers, triangleUVCount,
                                 sizeof(RTTriangleUVData), L"RT_TriangleUVBuffer") ||
        !EnsureFrameUploadBuffer(dev5, m_trianglePositionBuffers, trianglePositionCount,
                                 sizeof(RTTrianglePositionData), L"RT_TrianglePositionBuffer"))
    {
        core::Log::Error("PathTracer dispatch skipped: failed to prepare RT geometry buffers");
        return;
    }

    // --- Upload materials ---
    // No clamp: the buffer is guaranteed to hold materialCount entries above.
    // Clamping here while DispatchRays still covered every instance is what made
    // g_Materials[InstanceID()] read past the allocation for instance 256+.
    if (materialCount > 0 && materials)
        memcpy(m_materialBuffers.mapped[m_frameIndex], materials, sizeof(RTMaterialData) * materialCount);

    memcpy(m_instanceDataBuffers.mapped[m_frameIndex], instanceData, sizeof(RTInstanceData) * instanceDataCount);
    memcpy(m_triangleNormalBuffers.mapped[m_frameIndex], triangleNormals, sizeof(RTTriangleNormalData) * triangleNormalCount);
    memcpy(m_triangleUVBuffers.mapped[m_frameIndex], triangleUVs, sizeof(RTTriangleUVData) * triangleUVCount);
    memcpy(m_trianglePositionBuffers.mapped[m_frameIndex], trianglePositions, sizeof(RTTrianglePositionData) * trianglePositionCount);
    UpdateTextureDescriptors(device.Device5(), albedoTextures, albedoTextureCount,
                             kRTAlbedoDescriptorBase, kMaxRTAlbedoTextures,
                             m_boundAlbedoTextureCount, m_boundAlbedoTextureResources.data());
    UpdateTextureDescriptors(device.Device5(), normalTextures, normalTextureCount,
                             kRTNormalDescriptorBase, kMaxRTNormalTextures,
                             m_boundNormalTextureCount, m_boundNormalTextureResources.data());

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
        m_materialBuffers.buffer[m_frameIndex]->GetGPUVirtualAddress());
    // [4] Triangle normal buffer
    cmd->SetComputeRootShaderResourceView(4,
        m_triangleNormalBuffers.buffer[m_frameIndex]->GetGPUVirtualAddress());
    // [5] Instance metadata buffer
    cmd->SetComputeRootShaderResourceView(5,
        m_instanceDataBuffers.buffer[m_frameIndex]->GetGPUVirtualAddress());
    // [6] Triangle UV buffer
    cmd->SetComputeRootShaderResourceView(6,
        m_triangleUVBuffers.buffer[m_frameIndex]->GetGPUVirtualAddress());
    // [7] Triangle position buffer
    cmd->SetComputeRootShaderResourceView(7,
        m_trianglePositionBuffers.buffer[m_frameIndex]->GetGPUVirtualAddress());
    // [8] Material texture descriptor table (albedo, then normal)
    D3D12_GPU_DESCRIPTOR_HANDLE textureTable = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    textureTable.ptr += static_cast<UINT64>(kRTAlbedoDescriptorBase) * m_srvUavDescSize;
    cmd->SetComputeRootDescriptorTable(8, textureTable);

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
    auto* cmd = device.CmdList();

    if (!m_displayTexture)
    {
        // The caller already transitioned the back buffer PRESENT -> RENDER_TARGET in
        // anticipation of the copy. Returning without undoing that would leave it in
        // RENDER_TARGET at Present time (and with the overlay off, nothing else
        // restores it). Put it back so the frame still presents legally.
        D3D12_RESOURCE_BARRIER restore   = {};
        restore.Type                     = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        restore.Transition.pResource     = device.CurrentBackBuffer();
        restore.Transition.StateBefore   = D3D12_RESOURCE_STATE_RENDER_TARGET;
        restore.Transition.StateAfter    = D3D12_RESOURCE_STATE_PRESENT;
        restore.Transition.Subresource   = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (restore.Transition.pResource)
            cmd->ResourceBarrier(1, &restore);
        return;
    }

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
