// =============================================================================
// render/path_tracer.cpp — Path Tracer Implementation
// =============================================================================

#include "path_tracer.h"

#include <cmath>   // std::tan for the camera FOV term
#include "rt_texture_lod.h"   // PrimaryRayConeSpreadAngle for the ray-cone LOD
#include "../core/log.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <utility>

namespace render
{

static constexpr uint32_t kRTUavDescriptorCount = 2;
static constexpr uint32_t kRTAlbedoDescriptorBase = kRTUavDescriptorCount;
static constexpr uint32_t kRTNormalDescriptorBase = kRTAlbedoDescriptorBase + kMaxRTAlbedoTextures;
static constexpr uint32_t kRTOrmDescriptorBase    = kRTNormalDescriptorBase + kMaxRTNormalTextures;
static constexpr uint32_t kRTEmissiveDescriptorBase = kRTOrmDescriptorBase + kMaxRTOrmTextures;
// The prefiltered environment cubemap's SRV. ONE descriptor, after the four
// 64-entry material tables, so the heap goes 258 -> 259 per frame slot.
//
// The root signature's fifth range carries the TABLE-RELATIVE offset, which is
// this minus kRTAlbedoDescriptorBase (the index the table is actually bound at),
// not this value itself. The assertion below is what keeps the two arithmetics
// from drifting apart silently.
static constexpr uint32_t kRTEnvCubeDescriptorBase = kRTEmissiveDescriptorBase + kMaxRTEmissiveTextures;
static constexpr uint32_t kRTDescriptorsPerFrame   = kRTEnvCubeDescriptorBase + 1;
static_assert(kRTEnvCubeDescriptorBase == 258,
              "The env cube SRV must follow the four material tables at heap index 258");
static_assert(kRTDescriptorsPerFrame == 259,
              "RT descriptor heap is 259 per frame slot as of IBL Stage 4");
static_assert(kRTEnvCubeDescriptorBase - kRTAlbedoDescriptorBase ==
                  kMaxRTAlbedoTextures + kMaxRTNormalTextures + kMaxRTOrmTextures +
                  kMaxRTEmissiveTextures,
              "The env cube's table-relative offset in RTPipeline::CreateGlobalRootSignature "
              "must equal its heap index minus the index the table is bound at");

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
    if (!CreateIBLProbeResources(dev5)) return false;

    m_initialized = true;
    core::Log::Info("PathTracer initialized");
    return true;
}

void PathTracer::Shutdown()
{
    for (uint32_t i = 0; i < kFrameCount; i++)
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
    for (auto& heap : m_srvUavHeap) heap.Reset();

    // FrameUploadBuffer::Reset unmaps and releases every frame instance.
    m_materialBuffers.Reset();
    m_instanceDataBuffers.Reset();
    m_triangleNormalBuffers.Reset();
    m_triangleUVBuffers.Reset();
    m_trianglePositionBuffers.Reset();
    for (auto& cb : m_constantBuffer) cb.Reset();

    for (auto& probe : m_iblProbeBuffer) probe.Reset();
    m_iblProbeZeroUpload.Reset();
    m_iblProbeReadback.Reset();
    m_iblProbeReadbackPending = false;
    for (auto& cube : m_boundEnvCube) cube = nullptr;

    m_srvUavDescSize = 0;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        m_boundAlbedoTextureCount[i] = 0;
        m_boundAlbedoTextureResources[i].fill(nullptr);
        m_boundNormalTextureCount[i] = 0;
        m_boundNormalTextureResources[i].fill(nullptr);
        m_boundOrmTextureCount[i] = 0;
        m_boundOrmTextureResources[i].fill(nullptr);
        m_boundEmissiveTextureCount[i] = 0;
        m_boundEmissiveTextureResources[i].fill(nullptr);
    }
    m_accumFrameIndex = 0;
    m_hasPrevCamera = false;
    m_hasPrevQuality = false;
    m_hasPrevScene = false;
    m_initialized = false;
    core::Log::Info("PathTracer shut down");
}

// =============================================================================
// Create SRV/UAV Descriptor Heap
// =============================================================================
bool PathTracer::CreateDescriptorHeap(ID3D12Device5* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = kRTDescriptorsPerFrame;
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvUavHeap[i]));
        if (FAILED(hr))
        {
            core::Log::Errorf("Failed to create RT descriptor heap %u: 0x%08X", i, hr);
            for (auto& heap : m_srvUavHeap) heap.Reset();
            return false;
        }
        wchar_t name[32];
        swprintf_s(name, L"RT_SrvUavHeap[%u]", i);
        m_srvUavHeap[i]->SetName(name);
    }
    m_srvUavDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ClearMaterialTextureDescriptors(device);
    return true;
}

void PathTracer::ClearMaterialTextureDescriptors(ID3D12Device5* device)
{
    if (!device || m_srvUavDescSize == 0)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;

    auto clearRange = [&](ID3D12DescriptorHeap* heap,
                          uint32_t firstDescriptor,
                          uint32_t descriptorCount)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(firstDescriptor) * m_srvUavDescSize;
        for (uint32_t i = 0; i < descriptorCount; ++i)
        {
            device->CreateShaderResourceView(nullptr, &nullSrv, handle);
            handle.ptr += m_srvUavDescSize;
        }
    };

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (!m_srvUavHeap[i]) continue;
        clearRange(m_srvUavHeap[i].Get(), kRTAlbedoDescriptorBase, kMaxRTAlbedoTextures);
        clearRange(m_srvUavHeap[i].Get(), kRTNormalDescriptorBase, kMaxRTNormalTextures);
        clearRange(m_srvUavHeap[i].Get(), kRTOrmDescriptorBase, kMaxRTOrmTextures);
        clearRange(m_srvUavHeap[i].Get(), kRTEmissiveDescriptorBase, kMaxRTEmissiveTextures);

        // The env cube's slot needs a CUBE null SRV, not the TEXTURE2D one
        // above: a descriptor whose ViewDimension disagrees with the shader's
        // TextureCube declaration is not merely wrong, it is a debug-layer error.
        // The slot must hold something valid on every frame because the table is
        // bound unconditionally, and until EnvironmentIBL has baked, this is what
        // it holds. The shader never reads it - iblParams.z is 0 in exactly that
        // case - but "never read" is a property of the shader's control flow, and
        // leaving an invalid descriptor behind it would be relying on that.
        D3D12_SHADER_RESOURCE_VIEW_DESC nullCube = {};
        nullCube.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        nullCube.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURECUBE;
        nullCube.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullCube.TextureCube.MipLevels   = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE envHandle =
            m_srvUavHeap[i]->GetCPUDescriptorHandleForHeapStart();
        envHandle.ptr += static_cast<SIZE_T>(kRTEnvCubeDescriptorBase) * m_srvUavDescSize;
        device->CreateShaderResourceView(nullptr, &nullCube, envHandle);
        m_boundEnvCube[i] = nullptr;

        m_boundAlbedoTextureCount[i] = 0;
        m_boundAlbedoTextureResources[i].fill(nullptr);
        m_boundNormalTextureCount[i] = 0;
        m_boundNormalTextureResources[i].fill(nullptr);
        m_boundOrmTextureCount[i] = 0;
        m_boundOrmTextureResources[i].fill(nullptr);
        m_boundEmissiveTextureCount[i] = 0;
        m_boundEmissiveTextureResources[i].fill(nullptr);
    }
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
    if (!device || !m_srvUavHeap[m_frameIndex] || m_srvUavDescSize == 0)
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

    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_srvUavHeap[m_frameIndex]->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(firstDescriptor) * m_srvUavDescSize;

    for (uint32_t i = 0; i < maxDescriptors; ++i)
    {
        device->CreateShaderResourceView(nullptr, &nullSrv, handle);
        boundResources[i] = nullptr;
        handle.ptr += m_srvUavDescSize;
    }

    handle = m_srvUavHeap[m_frameIndex]->GetCPUDescriptorHandleForHeapStart();
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
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        device->CreateUnorderedAccessView(
            m_outputTexture.Get(), nullptr, &uavDesc,
            m_srvUavHeap[i]->GetCPUDescriptorHandleForHeapStart());

        // UAV slot 1: tone-mapped 8-bit display output
        D3D12_CPU_DESCRIPTOR_HANDLE displayHandle =
            m_srvUavHeap[i]->GetCPUDescriptorHandleForHeapStart();
        displayHandle.ptr += m_srvUavDescSize;
        uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateUnorderedAccessView(
            m_displayTexture.Get(), nullptr, &uavDesc, displayHandle);
        uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

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
    m_hasPrevScene = false;

    if (!ok)
    {
        // m_outputWidth/Height are now 0, so a later resize to any size retries
        // rather than early-outing. Dispatch() skips work while the textures are null.
        core::Log::Errorf("Path tracer resize to %ux%u failed; path tracing is disabled "
                          "until a later resize succeeds", width, height);
    }
    return ok;
}

void PathTracer::InvalidateAccumulation()
{
    m_accumFrameIndex = 0;
    m_hasPrevCamera = false;
    m_hasPrevQuality = false;
    m_hasPrevScene = false;
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

    for (uint32_t i = 0; i < kFrameCount; i++)
    {
        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_constantBuffer[i]));
        if (FAILED(hr)) return false;

        wchar_t name[16];
        swprintf_s(name, L"RT_CB[%u]", i);
        m_constantBuffer[i]->SetName(name);
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
// The DXR IBL consumption probe's resources
// =============================================================================
// Deliberately the same shape as Renderer::EnsureIBLConsumeProbeResources: a
// DEFAULT-heap block per frame in flight, one zero-filled UPLOAD buffer to clear
// it from, and one READBACK buffer. Same 64-byte layout, same reduction. Where
// the raster version differs is that this one is created in Init rather than
// lazily, because the path tracer has a single construction point.
bool PathTracer::CreateIBLProbeResources(ID3D12Device5* device)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = sizeof(IBLConsumeProbeBlock);
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        const HRESULT hr = device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&m_iblProbeBuffer[i]));
        if (FAILED(hr))
        {
            core::Log::Errorf("DXR IBL probe UAV allocation failed for slot %u: 0x%08X", i, hr);
            return false;
        }
        wchar_t name[64];
        swprintf_s(name, L"RT_IBLConsumeProbe[%u]", i);
        m_iblProbeBuffer[i]->SetName(name);
    }

    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    HRESULT hr = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_iblProbeZeroUpload));
    if (FAILED(hr))
    {
        core::Log::Errorf("DXR IBL probe clear upload allocation failed: 0x%08X", hr);
        return false;
    }
    m_iblProbeZeroUpload->SetName(L"RT_IBLConsumeProbeZeroUpload");

    void* mapped = nullptr;
    const D3D12_RANGE noRead = { 0, 0 };
    hr = m_iblProbeZeroUpload->Map(0, &noRead, &mapped);
    if (FAILED(hr) || !mapped)
    {
        core::Log::Errorf("DXR IBL probe clear upload map failed: 0x%08X", hr);
        return false;
    }
    std::memset(mapped, 0, sizeof(IBLConsumeProbeBlock));
    m_iblProbeZeroUpload->Unmap(0, nullptr);

    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    hr = device->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_iblProbeReadback));
    if (FAILED(hr))
    {
        core::Log::Errorf("DXR IBL probe readback allocation failed: 0x%08X", hr);
        return false;
    }
    m_iblProbeReadback->SetName(L"RT_IBLConsumeProbeReadback");
    return true;
}

// ZEROING IS THE WHOLE CONTRACT, exactly as on the raster side. Every word is an
// InterlockedMax or an InterlockedAdd from a zero start, so a block that was not
// cleared carries the previously probed dispatch's answer - and on the control
// frame the previously probed dispatch is the LIVE one, which would make the
// control pass while reporting the opposite of the truth.
bool PathTracer::PrepareIBLProbe(D3D12Device& device)
{
    const uint32_t slot = device.FrameIndex();
    if (!m_iblProbeBuffer[slot] || !m_iblProbeZeroUpload)
    {
        core::Log::Error("DXR IBL probe resources are unavailable");
        return false;
    }

    auto* cmd = device.CmdList();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_iblProbeBuffer[slot].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    cmd->ResourceBarrier(1, &barrier);
    cmd->CopyBufferRegion(m_iblProbeBuffer[slot].Get(), 0,
                          m_iblProbeZeroUpload.Get(), 0, sizeof(IBLConsumeProbeBlock));
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd->ResourceBarrier(1, &barrier);
    return true;
}

bool PathTracer::RecordIBLProbeReadback(D3D12Device& device)
{
    const uint32_t slot = device.FrameIndex();
    if (!m_iblProbeBuffer[slot] || !m_iblProbeReadback)
        return false;

    auto* cmd = device.CmdList();
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_iblProbeBuffer[slot].Get();
    cmd->ResourceBarrier(1, &uavBarrier);

    D3D12_RESOURCE_BARRIER transition = {};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition.pResource   = m_iblProbeBuffer[slot].Get();
    transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &transition);
    cmd->CopyBufferRegion(m_iblProbeReadback.Get(), 0,
                          m_iblProbeBuffer[slot].Get(), 0,
                          sizeof(IBLConsumeProbeBlock));
    std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
    cmd->ResourceBarrier(1, &transition);
    m_iblProbeReadbackPending = true;
    return true;
}

bool PathTracer::ReadIBLProbe(IBLConsumeValidation& validation, bool iblExpectedActive)
{
    validation = {};
    if (!m_iblProbeReadbackPending || !m_iblProbeReadback) return false;
    m_iblProbeReadbackPending = false;

    const D3D12_RANGE readRange = { 0, sizeof(IBLConsumeProbeBlock) };
    void* mapped = nullptr;
    const HRESULT hr = m_iblProbeReadback->Map(0, &readRange, &mapped);
    if (FAILED(hr) || !mapped)
    {
        core::Log::Errorf("DXR IBL probe readback map failed: 0x%08X", hr);
        return false;
    }

    IBLConsumeProbeBlock block = {};
    std::memcpy(&block, mapped, sizeof(block));

    const D3D12_RANGE noWrite = { 0, 0 };
    m_iblProbeReadback->Unmap(0, &noWrite);

    // THE SAME SHIPPED REDUCTION the raster probe uses. Not a copy of it - the
    // verdict logic has one definition, covered by the CPU cases in
    // tests/test_ibl_consume_probe.cpp, and both paths' markers are therefore
    // the same numbers computed the same way.
    validation = ReduceIBLConsumeProbe(block, iblExpectedActive);
    return true;
}

// Writes the cube's SRV into this frame slot's reserved descriptor. Returns
// whether a real cube landed there - the single input to iblParams.z, so a cube
// that failed to bake switches the environment off rather than leaving the
// shader to sample a null descriptor and shade from black.
bool PathTracer::UpdateEnvironmentDescriptor(ID3D12Device5* device, const EnvironmentIBL* ibl)
{
    if (!device || m_srvUavDescSize == 0 || !m_srvUavHeap[m_frameIndex])
        return false;
    if (!ibl || !ibl->IsBuilt() || !ibl->Cube())
        return false;

    // Rewritten only when the resource changes. EnvironmentIBL rebakes behind a
    // REVISION COUNTER and can in principle hand back a different resource, so
    // this compares the pointer rather than assuming a one-time write.
    if (m_boundEnvCube[m_frameIndex] == ibl->Cube())
        return true;

    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        m_srvUavHeap[m_frameIndex]->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(kRTEnvCubeDescriptorBase) * m_srvUavDescSize;

    // EnvironmentIBL owns the view description - format, mip count, cube
    // dimension - so the raster and DXR descriptors are written by ONE function
    // and cannot drift into describing the same resource two ways.
    ibl->WriteCubeSRV(device, handle);
    m_boundEnvCube[m_frameIndex] = ibl->Cube();
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
bool PathTracer::EnsureFrameUploadBuffer(D3D12Device& device,
                                         FrameUploadBuffer& target,
                                         uint32_t elementCount,
                                         uint64_t elementSize,
                                         const wchar_t* debugName)
{
    if (elementCount == 0) return false;
    if (target.Valid() && elementCount <= target.capacity) return true;

    // Same headroom the per-buffer versions used, so growth still amortises.
    const uint32_t newCapacity = elementCount + 64;
    const uint64_t byteSize = elementSize * static_cast<uint64_t>(newCapacity);
    FrameUploadBuffer replacement;

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        wchar_t name[96];
        swprintf_s(name, L"%s[%u]", debugName, i);
        if (!CreateMappedUploadBuffer(device.Device5(), byteSize, name,
                                      replacement.buffer[i], &replacement.mapped[i]))
        {
            replacement.Reset();
            return false;
        }
    }

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (target.buffer[i] && target.mapped[i])
            target.buffer[i]->Unmap(0, nullptr);
        target.mapped[i] = nullptr;
        device.DeferredRelease(target.buffer[i]);
        target.buffer[i] = std::move(replacement.buffer[i]);
        target.mapped[i] = replacement.mapped[i];
        replacement.mapped[i] = nullptr;
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
    const Texture* const* ormTextures,
    uint32_t ormTextureCount,
    const Texture* const* emissiveTextures,
    uint32_t emissiveTextureCount,
    uint32_t instanceCount,
    uint64_t sceneSignature,
    RTQualityMode qualityMode,
    const RTEnvironmentInputs& environment)
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

    const bool sceneChanged = !m_hasPrevScene ||
        sceneSignature != m_prevSceneSignature;

    if (cameraChanged || qualityChanged || sceneChanged)
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
    m_prevSceneSignature = sceneSignature;
    m_hasPrevScene = true;

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
    // Sourced from the camera so the DXR frustum cannot drift from the raster one.
    cb.tanHalfFovY   = std::tan(camera.GetFOV() * (3.14159265358979323846f / 180.0f) * 0.5f);
    // Ray-cone texture LOD: one pixel of output height of angular spread per unit
    // distance. Derived from the SAME tanHalfFovY the ray directions are built
    // from, so the cone can never describe a different frustum than the rays.
    cb.primaryConeSpread = PrimaryRayConeSpreadAngle(cb.tanHalfFovY, m_outputHeight);

    // -------------------------------------------------------------------------
    // Image-based lighting, for the STABLE PREVIEW.
    // -------------------------------------------------------------------------
    // The descriptor is written first, because iblParams.z is a claim about what
    // is actually bound at t0/space8 on THIS frame slot, not about what the
    // Renderer believes it baked.
    const bool envBound = UpdateEnvironmentDescriptor(device.Device5(), environment.ibl);

    if (envBound)
    {
        const core::SHColor9& sh = environment.ibl->IrradianceCoefficients();
        for (uint32_t i = 0; i < 9; ++i)
        {
            cb.iblSH[i][0] = sh.c[i].x;
            cb.iblSH[i][1] = sh.c[i].y;
            cb.iblSH[i][2] = sh.c[i].z;
            cb.iblSH[i][3] = 0.0f;
        }
    }

    cb.iblParams[0] = static_cast<float>(kEnvCubeMips);
    cb.iblParams[1] = environment.intensity;
    // TWO inputs, matching Renderer::BeginFrame's iblParams.z exactly: the cube
    // must really be bound, AND the negative control must not have switched it
    // off for this dispatch. A kill switch no code path can enter is not a
    // reachable state, and an unreachable state cannot be the control frame.
    cb.iblParams[2] = (envBound && !environment.disabled) ? 1.0f : 0.0f;
    // The probe write gate. Raised on the two probe dispatches only.
    cb.iblParams[3] = environment.probeWrite ? 1.0f : 0.0f;

    // Fires on the first dispatch and on any resize or FOV change, not per frame.
    // A spread of exactly 0 means every hit would clamp to mip 0 - the defect
    // ray cones replaced - so it is worth being able to see the number.
    if (cb.primaryConeSpread != m_loggedConeSpread)
    {
        m_loggedConeSpread = cb.primaryConeSpread;
        core::Log::Infof(
            "[SMOKE] rt_texture_lod=ray_cone primary_cone_spread=%.8f render_height=%u",
            static_cast<double>(cb.primaryConeSpread), m_outputHeight);
    }

    memcpy(m_cbMapped[m_frameIndex], &cb, sizeof(cb));

    if (!EnsureFrameUploadBuffer(device, m_materialBuffers, materialCount,
                                 sizeof(RTMaterialData), L"RT_MaterialBuffer") ||
        !EnsureFrameUploadBuffer(device, m_instanceDataBuffers, instanceDataCount,
                                 sizeof(RTInstanceData), L"RT_InstanceDataBuffer") ||
        !EnsureFrameUploadBuffer(device, m_triangleNormalBuffers, triangleNormalCount,
                                 sizeof(RTTriangleNormalData), L"RT_TriangleNormalBuffer") ||
        !EnsureFrameUploadBuffer(device, m_triangleUVBuffers, triangleUVCount,
                                 sizeof(RTTriangleUVData), L"RT_TriangleUVBuffer") ||
        !EnsureFrameUploadBuffer(device, m_trianglePositionBuffers, trianglePositionCount,
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
                             m_boundAlbedoTextureCount[m_frameIndex],
                             m_boundAlbedoTextureResources[m_frameIndex].data());
    UpdateTextureDescriptors(device.Device5(), normalTextures, normalTextureCount,
                             kRTNormalDescriptorBase, kMaxRTNormalTextures,
                             m_boundNormalTextureCount[m_frameIndex],
                             m_boundNormalTextureResources[m_frameIndex].data());
    UpdateTextureDescriptors(device.Device5(), ormTextures, ormTextureCount,
                             kRTOrmDescriptorBase, kMaxRTOrmTextures,
                             m_boundOrmTextureCount[m_frameIndex],
                             m_boundOrmTextureResources[m_frameIndex].data());
    UpdateTextureDescriptors(device.Device5(), emissiveTextures, emissiveTextureCount,
                             kRTEmissiveDescriptorBase, kMaxRTEmissiveTextures,
                             m_boundEmissiveTextureCount[m_frameIndex],
                             m_boundEmissiveTextureResources[m_frameIndex].data());

    // --- Set up for DispatchRays ---
    cmd->SetComputeRootSignature(m_pipeline.GetGlobalRootSig());
    cmd->SetPipelineState1(m_pipeline.GetStateObject());

    // Set descriptor heap
    ID3D12DescriptorHeap* heaps[] = { m_srvUavHeap[m_frameIndex].Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    // Bind global root parameters
    // [0] TLAS SRV
    cmd->SetComputeRootShaderResourceView(0, m_accel.GetTLASAddress());
    // [1] Output UAV descriptor table
    cmd->SetComputeRootDescriptorTable(1,
        m_srvUavHeap[m_frameIndex]->GetGPUDescriptorHandleForHeapStart());
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
    // [8] Material texture descriptor table (albedo, then normal, then ORM).
    // The table base is the albedo base; the ranges declared in the root
    // signature carry the offsets, so this stays a single bind.
    D3D12_GPU_DESCRIPTOR_HANDLE textureTable =
        m_srvUavHeap[m_frameIndex]->GetGPUDescriptorHandleForHeapStart();
    textureTable.ptr += static_cast<UINT64>(kRTAlbedoDescriptorBase) * m_srvUavDescSize;
    cmd->SetComputeRootDescriptorTable(8, textureTable);
    // [9] The IBL consumption probe's raw buffer. Bound on EVERY dispatch: a
    // root descriptor cannot be left unset once the shader declares the
    // resource, so the gate is iblParams.w inside the shader, not this bind.
    if (m_iblProbeBuffer[m_frameIndex])
        cmd->SetComputeRootUnorderedAccessView(
            9, m_iblProbeBuffer[m_frameIndex]->GetGPUVirtualAddress());

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
