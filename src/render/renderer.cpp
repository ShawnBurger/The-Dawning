// =============================================================================
// render/renderer.cpp — Renderer Implementation
// =============================================================================

#include "renderer.h"
#include "shader_utils.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdio>   // snprintf, for the MAX_RASTER_TEXTURES shader define

namespace render
{

// =============================================================================
// Init
// =============================================================================
bool Renderer::Init(D3D12Device& device)
{
    if (!CreateRootSignature(device.Device())) return false;
    if (!CreatePSO(device.Device()))           return false;
    if (!CreateSkyPSO(device.Device()))        return false;
    if (!CreateConstantBuffers(device.Device())) return false;
    if (!CreateTextureHeap(device.Device())) return false;
    if (!CreateHDRTarget(device.Device(),
                         static_cast<uint32_t>(device.Width()),
                         static_cast<uint32_t>(device.Height()))) return false;
    if (!CreateTonemapPipeline(device.Device())) return false;
    if (!CreateBloomPipeline(device.Device())) return false;
    if (!CreateShadowResources(device.Device())) return false;
    if (!CreateShadowPSO(device.Device())) return false;

    core::Log::Info("Renderer initialized");
    return true;
}

void Renderer::Shutdown()
{
    // Unmap all CB buffers
    for (uint32_t i = 0; i < kFrameCount; i++)
    {
        if (m_cbUploadBuffers[i])
        {
            m_cbUploadBuffers[i]->Unmap(0, nullptr);
            m_cbMappedPtrs[i] = nullptr;
        }
    }
    m_bloomBlurPSO.Reset();
    m_bloomPrefilterPSO.Reset();
    m_bloomRootSig.Reset();
    m_bloomRtvHeap.Reset();
    for (auto& t : m_bloomTarget) t.Reset();
    m_bloomWidth = 0;
    m_bloomHeight = 0;
    m_tonemapPSO.Reset();
    m_tonemapRootSig.Reset();
    m_hdrSrvHeap.Reset();
    m_hdrRtvHeap.Reset();
    m_hdrTarget.Reset();
    m_hdrWidth = 0;
    m_hdrHeight = 0;
    m_hdrIsRenderTarget = false;
    m_textureHeap.Reset();
    m_skyPSO.Reset();
    m_pso.Reset();
    m_rootSig.Reset();
    m_textureDescSize = 0;
    m_textureAllocator.ReclaimAll();
    core::Log::Info("Renderer shut down");
}

// =============================================================================
// HDR scene target
// =============================================================================
bool Renderer::CreateHDRTarget(ID3D12Device* device, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        core::Log::Error("HDR target requires non-zero dimensions");
        return false;
    }

    m_hdrTarget.Reset();
    m_hdrIsRenderTarget = false;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = kHDRFormat;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // Must match the ClearRenderTargetView colour below, or the driver loses its
    // fast-clear path and the debug layer warns on every clear.
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format   = kHDRFormat;
    clearValue.Color[0] = kSceneClearColor[0];
    clearValue.Color[1] = kSceneClearColor[1];
    clearValue.Color[2] = kSceneClearColor[2];
    clearValue.Color[3] = kSceneClearColor[3];

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
        IID_PPV_ARGS(&m_hdrTarget));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create HDR scene target %ux%u: 0x%08X", width, height, hr);
        m_hdrWidth = 0;
        m_hdrHeight = 0;
        return false;
    }
    m_hdrTarget->SetName(L"HDRSceneTarget");

    if (!m_hdrRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors = 1;
        rtvDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_hdrRtvHeap));
        if (FAILED(hr))
        {
            core::Log::Errorf("Failed to create HDR RTV heap: 0x%08X", hr);
            m_hdrTarget.Reset();
            m_hdrWidth = 0;
            m_hdrHeight = 0;
            return false;
        }
        m_hdrRtvHeap->SetName(L"HDRSceneRTVHeap");
    }

    if (!m_hdrSrvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
        srvDesc.NumDescriptors = 3;   // scene HDR + bloom A + bloom B
        srvDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_hdrSrvHeap));
        if (FAILED(hr))
        {
            core::Log::Errorf("Failed to create HDR SRV heap: 0x%08X", hr);
            m_hdrTarget.Reset();
            m_hdrWidth = 0;
            m_hdrHeight = 0;
            return false;
        }
        m_hdrSrvHeap->SetName(L"HDRSceneSRVHeap");
    }

    // Views name the resource, so they are recreated on every resize.
    device->CreateRenderTargetView(m_hdrTarget.Get(), nullptr,
                                   m_hdrRtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                  = kHDRFormat;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(m_hdrTarget.Get(), &srv,
                                     m_hdrSrvHeap->GetCPUDescriptorHandleForHeapStart());

    // ---- Bloom ping-pong pair at half resolution ----
    m_bloomWidth  = (width  + 1) / 2;
    m_bloomHeight = (height + 1) / 2;

    D3D12_RESOURCE_DESC bloomDesc = desc;
    bloomDesc.Width  = m_bloomWidth;
    bloomDesc.Height = m_bloomHeight;

    D3D12_CLEAR_VALUE bloomClear = {};
    bloomClear.Format = kHDRFormat;   // black; bloom starts from nothing

    if (!m_bloomRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC bloomRtvDesc = {};
        bloomRtvDesc.NumDescriptors = kBloomTargetCount;
        bloomRtvDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        bloomRtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = device->CreateDescriptorHeap(&bloomRtvDesc, IID_PPV_ARGS(&m_bloomRtvHeap));
        if (FAILED(hr))
        {
            core::Log::Errorf("Failed to create bloom RTV heap: 0x%08X", hr);
            return false;
        }
        m_bloomRtvHeap->SetName(L"BloomRTVHeap");
    }

    const uint32_t rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const uint32_t srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (uint32_t i = 0; i < kBloomTargetCount; ++i)
    {
        m_bloomTarget[i].Reset();
        hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bloomDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &bloomClear,
            IID_PPV_ARGS(&m_bloomTarget[i]));
        if (FAILED(hr))
        {
            core::Log::Errorf("Failed to create bloom target %u (%ux%u): 0x%08X",
                              i, m_bloomWidth, m_bloomHeight, hr);
            return false;
        }
        wchar_t bloomName[32];
        swprintf_s(bloomName, L"BloomTarget[%u]", i);
        m_bloomTarget[i]->SetName(bloomName);
        m_bloomIsRenderTarget[i] = false;

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_bloomRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += static_cast<SIZE_T>(i) * rtvStride;
        device->CreateRenderTargetView(m_bloomTarget[i].Get(), nullptr, rtvHandle);

        // Descriptors 1 and 2 of the shared SRV heap; 0 is the scene target.
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_hdrSrvHeap->GetCPUDescriptorHandleForHeapStart();
        srvHandle.ptr += static_cast<SIZE_T>(1 + i) * srvStride;
        device->CreateShaderResourceView(m_bloomTarget[i].Get(), &srv, srvHandle);
    }

    m_hdrWidth  = width;
    m_hdrHeight = height;
    core::Log::Infof("HDR scene target created: %ux%u (R16G16B16A16_FLOAT), bloom %ux%u",
                     width, height, m_bloomWidth, m_bloomHeight);
    return true;
}

bool Renderer::ResizeHDRTarget(D3D12Device& device, uint32_t width, uint32_t height)
{
    if (width == m_hdrWidth && height == m_hdrHeight && m_hdrTarget) return true;

    // The outgoing target may still be referenced by frames in flight, so it goes
    // through the fence-guarded queue rather than being dropped here.
    if (m_hdrTarget)
        device.DeferredRelease(m_hdrTarget);
    for (uint32_t i = 0; i < kBloomTargetCount; ++i)
        if (m_bloomTarget[i])
            device.DeferredRelease(m_bloomTarget[i]);

    return CreateHDRTarget(device.Device(), width, height);
}

// =============================================================================
// Tone-map resolve pipeline
// =============================================================================
bool Renderer::CreateTonemapPipeline(ID3D12Device* device)
{
    auto vsBytecode = CompileShaderFromFile(L"shaders/tonemap_vs.hlsl", "main", "vs_5_1");
    auto psBytecode = CompileShaderFromFile(L"shaders/tonemap_ps.hlsl", "main", "ps_5_1");
    if (!vsBytecode || !psBytecode)
    {
        core::Log::Error("Failed to compile tone-map shaders");
        return false;
    }

    // Its own minimal root signature: one SRV table plus a point sampler.
    // Reusing the material root signature would drag along three constant
    // buffers this pass has no use for.
    // Two descriptors: t0 = scene HDR, t1 = bloom. They are adjacent in the
    // shared SRV heap by construction (0, 1), so one range covers both.
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 2;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParams[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // exposure + bloomIntensity + padding. Root constants, so exposure can
    // change per frame without touching the constant-buffer ring.
    rootParams[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[1].Constants.ShaderRegister = 0;
    rootParams[1].Constants.RegisterSpace  = 0;
    rootParams[1].Constants.Num32BitValues = 4;
    rootParams[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    // s0 point for the 1:1 scene resolve, s1 linear for the half-res bloom
    // upsample. Filtering the scene would soften a resolve that is not scaling;
    // point-sampling the bloom would reintroduce the blockiness the blur removed.
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister   = 0;
    samplers[0].RegisterSpace    = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[1] = samplers[0];
    samplers[1].Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = _countof(rootParams);
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = _countof(samplers);
    rsDesc.pStaticSamplers   = samplers;
    rsDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> sigBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &sigBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob)
            core::Log::Errorf("Tone-map root signature error: %s",
                              static_cast<const char*>(errorBlob->GetBufferPointer()));
        return false;
    }

    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                     sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_tonemapRootSig));
    if (FAILED(hr))
    {
        core::Log::Errorf("Tone-map CreateRootSignature failed: 0x%08X", hr);
        return false;
    }
    m_tonemapRootSig->SetName(L"TonemapRootSignature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature        = m_tonemapRootSig.Get();
    psoDesc.VS.pShaderBytecode    = vsBytecode->GetBufferPointer();
    psoDesc.VS.BytecodeLength     = vsBytecode->GetBufferSize();
    psoDesc.PS.pShaderBytecode    = psBytecode->GetBufferPointer();
    psoDesc.PS.BytecodeLength     = psBytecode->GetBufferSize();
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable   = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0]    = DXGI_FORMAT_R8G8B8A8_UNORM;   // the back buffer
    psoDesc.SampleDesc       = { 1, 0 };
    psoDesc.SampleMask       = UINT_MAX;

    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_tonemapPSO));
    if (FAILED(hr))
    {
        core::Log::Errorf("Tone-map CreateGraphicsPipelineState failed: 0x%08X", hr);
        return false;
    }
    m_tonemapPSO->SetName(L"TonemapPSO");
    core::Log::Info("[SMOKE] tonemap_pso=ok");
    core::Log::Info("Tone-map resolve pipeline created");
    return true;
}

// =============================================================================
// Bloom
// =============================================================================
bool Renderer::CreateBloomPipeline(ID3D12Device* device)
{
    auto vs          = CompileShaderFromFile(L"shaders/bloom_vs.hlsl", "main", "vs_5_1");
    auto prefilterPs = CompileShaderFromFile(L"shaders/bloom_prefilter_ps.hlsl", "main", "ps_5_1");
    auto blurPs      = CompileShaderFromFile(L"shaders/bloom_blur_ps.hlsl", "main", "ps_5_1");
    if (!vs || !prefilterPs || !blurPs)
    {
        core::Log::Error("Failed to compile bloom shaders");
        return false;
    }

    // One SRV table plus root constants. Root constants rather than a constant
    // buffer because the payload is 8 DWORDs and changes per pass - a CB would
    // mean three ring allocations a frame for no benefit.
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 8;
    params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters     = _countof(params);
    rs.pParameters       = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers   = &sampler;
    rs.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> sigBlob;
    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob)
            core::Log::Errorf("Bloom root signature error: %s",
                              static_cast<const char*>(errBlob->GetBufferPointer()));
        return false;
    }
    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_bloomRootSig));
    if (FAILED(hr))
    {
        core::Log::Errorf("Bloom CreateRootSignature failed: 0x%08X", hr);
        return false;
    }
    m_bloomRootSig->SetName(L"BloomRootSignature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_bloomRootSig.Get();
    pso.VS.pShaderBytecode    = vs->GetBufferPointer();
    pso.VS.BytecodeLength     = vs->GetBufferSize();
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0]    = kHDRFormat;   // bloom stays linear HDR throughout
    pso.SampleDesc       = { 1, 0 };
    pso.SampleMask       = UINT_MAX;

    pso.PS.pShaderBytecode = prefilterPs->GetBufferPointer();
    pso.PS.BytecodeLength  = prefilterPs->GetBufferSize();
    hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_bloomPrefilterPSO));
    if (FAILED(hr))
    {
        core::Log::Errorf("Bloom prefilter PSO failed: 0x%08X", hr);
        return false;
    }
    m_bloomPrefilterPSO->SetName(L"BloomPrefilterPSO");

    pso.PS.pShaderBytecode = blurPs->GetBufferPointer();
    pso.PS.BytecodeLength  = blurPs->GetBufferSize();
    hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_bloomBlurPSO));
    if (FAILED(hr))
    {
        core::Log::Errorf("Bloom blur PSO failed: 0x%08X", hr);
        return false;
    }
    m_bloomBlurPSO->SetName(L"BloomBlurPSO");

    core::Log::Info("[SMOKE] bloom_pso=ok");
    core::Log::Info("Bloom pipeline created (half-res prefilter + separable blur)");
    return true;
}

// Prefilter scene into A, blur A->B horizontally, blur B->A vertically. The
// result is left in A (SRV descriptor 1) so ResolveToBackBuffer can bind the
// shared heap at descriptor 0 and receive t0 = scene, t1 = bloom.
void Renderer::RenderBloom(D3D12Device& device)
{
    if (!m_bloomPrefilterPSO || !m_bloomBlurPSO || !m_bloomTarget[0] || !m_bloomTarget[1])
        return;

    auto* cmd = device.CmdList();
    const uint32_t rtvStride =
        device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const uint32_t srvStride =
        device.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f,
                          static_cast<float>(m_bloomWidth),
                          static_cast<float>(m_bloomHeight), 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0,
                      static_cast<LONG>(m_bloomWidth),
                      static_cast<LONG>(m_bloomHeight) };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    ID3D12DescriptorHeap* heaps[] = { m_hdrSrvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_bloomRootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);

    struct BloomConstants
    {
        float texelSize[2];
        float threshold;
        float softKnee;
        float intensity;
        float pad[3];
    };

    auto srvAt = [&](uint32_t i) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_hdrSrvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(i) * srvStride;
        return h;
    };
    auto rtvAt = [&](uint32_t i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_bloomRtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * rtvStride;
        return h;
    };
    auto toRenderTarget = [&](uint32_t i) {
        if (!m_bloomIsRenderTarget[i])
        {
            device.TransitionResource(m_bloomTarget[i].Get(),
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_bloomIsRenderTarget[i] = true;
        }
    };
    auto toShaderResource = [&](uint32_t i) {
        if (m_bloomIsRenderTarget[i])
        {
            device.TransitionResource(m_bloomTarget[i].Get(),
                                      D3D12_RESOURCE_STATE_RENDER_TARGET,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_bloomIsRenderTarget[i] = false;
        }
    };

    // Pass 1: prefilter scene -> A. texelSize is the SOURCE's, i.e. full res.
    {
        BloomConstants k = {};
        k.texelSize[0] = 1.0f / static_cast<float>(m_hdrWidth  ? m_hdrWidth  : 1u);
        k.texelSize[1] = 1.0f / static_cast<float>(m_hdrHeight ? m_hdrHeight : 1u);
        k.threshold = m_bloomThreshold;
        k.softKnee  = m_bloomSoftKnee;
        k.intensity = m_bloomIntensity;

        toRenderTarget(0);
        auto rtv = rtvAt(0);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd->SetPipelineState(m_bloomPrefilterPSO.Get());
        cmd->SetGraphicsRootDescriptorTable(0, srvAt(0));   // scene HDR
        cmd->SetGraphicsRoot32BitConstants(1, 8, &k, 0);
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    const float invBloomW = 1.0f / static_cast<float>(m_bloomWidth  ? m_bloomWidth  : 1u);
    const float invBloomH = 1.0f / static_cast<float>(m_bloomHeight ? m_bloomHeight : 1u);

    // Blur iterations, each a horizontal then a vertical pass. A single pair
    // reaches only about 3 half-res texels, i.e. a ~6 pixel glow at full
    // resolution - measurably correct but far too tight to read as bloom. Each
    // iteration doubles the tap spacing, so kIterations passes widen the reach
    // geometrically (3 -> 6 -> 12 texels) for the cost of two more fullscreen
    // draws at quarter the pixel count.
    //
    // Widening by spacing rather than by more taps keeps the sample count fixed;
    // the gaps it leaves are filled because each iteration blurs the ALREADY
    // blurred result, not the original.
    //
    // An H+V pair returns the result to A, so after any number of complete
    // iterations the final image is in A, at SRV descriptor 1, which is what the
    // resolve expects.
    constexpr uint32_t kIterations = 3;
    for (uint32_t iter = 0; iter < kIterations; ++iter)
    {
        const float spread = static_cast<float>(1u << iter);

        // Horizontal: A -> B
        {
            BloomConstants k = {};
            k.texelSize[0] = invBloomW * spread;
            k.texelSize[1] = 0.0f;
            toShaderResource(0);
            toRenderTarget(1);
            auto rtv = rtvAt(1);
            cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            cmd->SetPipelineState(m_bloomBlurPSO.Get());
            cmd->SetGraphicsRootDescriptorTable(0, srvAt(1));   // bloom A
            cmd->SetGraphicsRoot32BitConstants(1, 8, &k, 0);
            cmd->DrawInstanced(3, 1, 0, 0);
        }

        // Vertical: B -> A
        {
            BloomConstants k = {};
            k.texelSize[0] = 0.0f;
            k.texelSize[1] = invBloomH * spread;
            toShaderResource(1);
            toRenderTarget(0);
            auto rtv = rtvAt(0);
            cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            cmd->SetGraphicsRootDescriptorTable(0, srvAt(2));   // bloom B
            cmd->SetGraphicsRoot32BitConstants(1, 8, &k, 0);
            cmd->DrawInstanced(3, 1, 0, 0);
        }
    }

    toShaderResource(0);   // A is now readable by the resolve
}

// =============================================================================
// Scene pass / resolve
// =============================================================================
void Renderer::BeginScenePass(D3D12Device& device)
{
    if (!m_hdrTarget) return;
    auto* cmd = device.CmdList();

    if (!m_hdrIsRenderTarget)
    {
        device.TransitionResource(m_hdrTarget.Get(),
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_hdrIsRenderTarget = true;
    }

    auto rtv = m_hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
    auto dsv = device.DSV();
    cmd->ClearRenderTargetView(rtv, kSceneClearColor, 0, nullptr);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
}

void Renderer::ResolveToBackBuffer(D3D12Device& device)
{
    if (!m_hdrTarget || !m_tonemapPSO) return;
    auto* cmd = device.CmdList();

    if (m_hdrIsRenderTarget)
    {
        device.TransitionResource(m_hdrTarget.Get(),
                                  D3D12_RESOURCE_STATE_RENDER_TARGET,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_hdrIsRenderTarget = false;
    }

    // Bloom runs here, between the scene pass and the resolve, reading the scene
    // target as an SRV. It rebinds viewport/scissor to half resolution, so the
    // full-res ones are restored below before the resolve draws.
    if (m_bloomIntensity > 0.0f)
        RenderBloom(device);

    // The back buffer becomes a render target only now, for the resolve itself.
    device.TransitionResource(device.CurrentBackBuffer(),
                              D3D12_RESOURCE_STATE_PRESENT,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_VIEWPORT fullVp = { 0.0f, 0.0f,
                              static_cast<float>(device.Width()),
                              static_cast<float>(device.Height()), 0.0f, 1.0f };
    D3D12_RECT fullSc = { 0, 0,
                          static_cast<LONG>(device.Width()),
                          static_cast<LONG>(device.Height()) };
    cmd->RSSetViewports(1, &fullVp);
    cmd->RSSetScissorRects(1, &fullSc);

    auto rtv = device.CurrentRTV();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);   // no depth: full overwrite

    ID3D12DescriptorHeap* heaps[] = { m_hdrSrvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_tonemapRootSig.Get());
    // Table starts at descriptor 0, so the shader sees t0 = scene HDR and
    // t1 = bloom A - which is exactly where RenderBloom leaves its result.
    cmd->SetGraphicsRootDescriptorTable(0, m_hdrSrvHeap->GetGPUDescriptorHandleForHeapStart());

    struct TonemapConstants
    {
        float exposure;
        float bloomIntensity;
        float pad[2];
    } tonemapConstants = {};
    tonemapConstants.exposure = m_exposure;
    // Zero here also makes the shader skip the bloom fetch entirely, so a
    // disabled bloom costs nothing rather than sampling a stale target.
    tonemapConstants.bloomIntensity = m_bloomIntensity;
    cmd->SetGraphicsRoot32BitConstants(1, 4, &tonemapConstants, 0);

    cmd->SetPipelineState(m_tonemapPSO.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    // Left in RENDER_TARGET so the overlay can draw over it.
}

// =============================================================================
// Root Signature — v1.1 with fallback to v1.0
// =============================================================================
// Layout (research-informed):
//   Slot 0: Root CBV at b0 (per-object) — 2 DWORDs, hot, changes every draw
//   Slot 1: Root CBV at b1 (per-frame)  — 2 DWORDs, warm, changes once/frame
//   Slot 2: Root CBV at b2 (material)   — 2 DWORDs, warm, changes per material
//   Slot 3: Descriptor table for material textures (t0-t127) — 1 DWORD
//   Slot 4: Descriptor table for the shadow map (t0, space1) — 1 DWORD
//   Static samplers at s0 (material) and s1 (shadow comparison) — free
// Total: 8 DWORDs of 64. The comment previously claimed 7 and listed only four
// slots (it predated the shadow table) while the success log below claimed 9;
// both were stale, and root-signature space is exactly the thing nobody wants
// to budget from a wrong number.
// =============================================================================
bool Renderer::CreateRootSignature(ID3D12Device* device)
{
    // Check v1.1 support
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE,
                &featureData, sizeof(featureData))))
    {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
        core::Log::Warn("Root signature v1.1 not supported, falling back to v1.0");
    }

    // Static sampler (free — doesn't cost root signature space)
    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.MipLODBias       = 0.0f;
    staticSampler.MaxAnisotropy    = 16;
    staticSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    staticSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSampler.MinLOD           = 0.0f;
    staticSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister   = 0;

    // s1: hardware depth comparison. The GPU compares against the reference
    // depth and bilinearly filters the RESULT of four comparisons, so a single
    // SampleCmpLevelZero already gives 2x2 percentage-closer filtering for the
    // cost of one tap. BORDER address mode with a white (1.0 = farthest) border
    // means anything outside the light frustum reads as fully lit rather than
    // fully shadowed - the demo scene is larger than kShadowExtent, and the
    // alternative is a hard black square around the shadowed region.
    D3D12_STATIC_SAMPLER_DESC shadowSampler = {};
    shadowSampler.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.MipLODBias       = 0.0f;
    shadowSampler.MaxAnisotropy    = 1;
    shadowSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MinLOD           = 0.0f;
    shadowSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister   = 1;
    shadowSampler.RegisterSpace    = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // These two assignments used to sit BELOW the array initialiser, so they
    // mutated a copy nothing ever read and s0 shipped as SHADER_VISIBILITY_ALL
    // rather than the intended PIXEL. Benign - ALL is a superset and
    // RegisterSpace 0 is the zero-init value - but the lines read as if they
    // took effect.
    staticSampler.RegisterSpace    = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    const D3D12_STATIC_SAMPLER_DESC staticSamplers[] = { staticSampler, shadowSampler };

    D3D12_ROOT_SIGNATURE_FLAGS flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
      | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
      | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
      | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> sigBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr;

    if (featureData.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1)
    {
        // v1.1: enables driver optimizations via DATA_STATIC / DESCRIPTORS_VOLATILE flags
        D3D12_DESCRIPTOR_RANGE1 textureRange = {};
        textureRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors                    = kMaxRasterTextures;
        textureRange.BaseShaderRegister                = 0;
        textureRange.RegisterSpace                     = 0;
        textureRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE1 shadowRange = {};
        shadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        shadowRange.NumDescriptors                    = 1;
        shadowRange.BaseShaderRegister                = 0;
        shadowRange.RegisterSpace                     = 1;
        shadowRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParams[5] = {};

        // Slot 0: Per-object CBV (b0) — changes every draw call (hottest)
        rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0;
        rootParams[0].Descriptor.RegisterSpace  = 0;
        rootParams[0].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        // Slot 1: Per-frame CBV (b1) — changes once per frame
        rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[1].Descriptor.ShaderRegister = 1;
        rootParams[1].Descriptor.RegisterSpace  = 0;
        rootParams[1].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        // Slot 2: Material CBV (b2) — changes per material
        rootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[2].Descriptor.ShaderRegister = 2;
        rootParams[2].Descriptor.RegisterSpace  = 0;
        rootParams[2].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        rootParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        // Slot 3: Material texture table (t0-t127)
        rootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[3].DescriptorTable.pDescriptorRanges   = &textureRange;
        rootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        // Slot 4: shadow map (t0, space1). A separate table from the material
        // one because it is bound at a fixed heap slot the allocator never hands
        // out, and because it must be valid before any material exists.
        rootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[4].DescriptorTable.pDescriptorRanges   = &shadowRange;
        rootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC vrsDesc = {};
        vrsDesc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
        vrsDesc.Desc_1_1.NumParameters     = _countof(rootParams);
        vrsDesc.Desc_1_1.pParameters       = rootParams;
        vrsDesc.Desc_1_1.NumStaticSamplers = _countof(staticSamplers);
        vrsDesc.Desc_1_1.pStaticSamplers   = staticSamplers;
        vrsDesc.Desc_1_1.Flags             = flags;

        hr = D3D12SerializeVersionedRootSignature(&vrsDesc, &sigBlob, &errorBlob);
    }
    else
    {
        // v1.0 fallback
        D3D12_DESCRIPTOR_RANGE textureRange = {};
        textureRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors                    = kMaxRasterTextures;
        textureRange.BaseShaderRegister                = 0;
        textureRange.RegisterSpace                     = 0;
        textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE shadowRange = {};
        shadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        shadowRange.NumDescriptors                    = 1;
        shadowRange.BaseShaderRegister                = 0;
        shadowRange.RegisterSpace                     = 1;
        shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParams[5] = {};

        rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0;
        rootParams[0].Descriptor.RegisterSpace  = 0;
        rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[1].Descriptor.ShaderRegister = 1;
        rootParams[1].Descriptor.RegisterSpace  = 0;
        rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[2].Descriptor.ShaderRegister = 2;
        rootParams[2].Descriptor.RegisterSpace  = 0;
        rootParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[3].DescriptorTable.pDescriptorRanges   = &textureRange;
        rootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[4].DescriptorTable.pDescriptorRanges   = &shadowRange;
        rootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters     = _countof(rootParams);
        rsDesc.pParameters       = rootParams;
        rsDesc.NumStaticSamplers = _countof(staticSamplers);
        rsDesc.pStaticSamplers   = staticSamplers;
        rsDesc.Flags             = flags;

        hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
    }
    if (FAILED(hr))
    {
        if (errorBlob)
            core::Log::Errorf("Root signature error: %s",
                              static_cast<const char*>(errorBlob->GetBufferPointer()));
        return false;
    }

    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                      sigBlob->GetBufferSize(),
                                      IID_PPV_ARGS(&m_rootSig));
    if (FAILED(hr))
    {
        core::Log::Errorf("CreateRootSignature failed: 0x%08X", hr);
        return false;
    }

    m_rootSig->SetName(L"MainRootSignature");
    core::Log::Infof("Root signature created (v%s, 3 root CBVs + raster texture table + shadow table, 2 static samplers, 8 DWORDs)",
                     featureData.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1 ? "1.1" : "1.0");
    return true;
}

// =============================================================================
// PSO — Classic API (stream upgrade in Layer 3)
// =============================================================================
bool Renderer::CreatePSO(ID3D12Device* device)
{
    // Compile shaders
    auto vsBytecode = CompileShaderFromFile(L"shaders/basic_vs.hlsl", "main", "vs_5_1");
    // Feed the heap size to HLSL rather than letting the shader hardcode it.
    // The material texture table's size lives in THREE places that must agree:
    // kMaxRasterTextures here, the root-signature SRV range NumDescriptors
    // (which already uses the constant), and the HLSL array declaration. The
    // third was a literal 128, so growing the heap would have produced a silent
    // C++/HLSL mismatch rather than a compile error. Now there is one source.
    char maxRasterTexturesText[16];
    snprintf(maxRasterTexturesText, sizeof(maxRasterTexturesText), "%u", kMaxRasterTextures);
    const D3D_SHADER_MACRO psDefines[] = {
        { "MAX_RASTER_TEXTURES", maxRasterTexturesText },
        { nullptr, nullptr }
    };

    auto psBytecode = CompileShaderFromFile(L"shaders/basic_ps.hlsl", "main", "ps_5_1", psDefines);

    if (!vsBytecode || !psBytecode)
    {
        core::Log::Error("Failed to compile shaders for main PSO");
        return false;
    }

    // PSO description
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig.Get();

    psoDesc.VS.pShaderBytecode = vsBytecode->GetBufferPointer();
    psoDesc.VS.BytecodeLength  = vsBytecode->GetBufferSize();
    psoDesc.PS.pShaderBytecode = psBytecode->GetBufferPointer();
    psoDesc.PS.BytecodeLength  = psBytecode->GetBufferSize();

    // Input layout
    psoDesc.InputLayout.pInputElementDescs = kVertexLayout;
    psoDesc.InputLayout.NumElements        = kVertexLayoutCount;

    // Rasterizer: solid fill, back-face culling, CW front (LH convention)
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE; // CW = front face (LH)
    psoDesc.RasterizerState.DepthBias             = 0;
    psoDesc.RasterizerState.DepthBiasClamp        = 0.0f;
    psoDesc.RasterizerState.SlopeScaledDepthBias  = 0.0f;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;
    psoDesc.RasterizerState.MultisampleEnable     = FALSE;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    psoDesc.RasterizerState.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // Blend state: opaque (no blending)
    psoDesc.BlendState.AlphaToCoverageEnable  = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable           = FALSE;
    psoDesc.BlendState.RenderTarget[0].LogicOpEnable         = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Depth-stencil: depth test on, write on
    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    // Output
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = kHDRFormat;   // linear scene target, not the back buffer
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc            = { 1, 0 };
    psoDesc.SampleMask            = UINT_MAX;

    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
    if (FAILED(hr))
    {
        core::Log::Errorf("CreateGraphicsPipelineState failed: 0x%08X", hr);
        return false;
    }

    m_pso->SetName(L"MainGraphicsPSO");
    core::Log::Info("Graphics PSO created (VS+PS, depth on, CW front, back-face cull)");
    return true;
}

bool Renderer::CreateSkyPSO(ID3D12Device* device)
{
    auto vsBytecode = CompileShaderFromFile(L"shaders/sky_vs.hlsl", "main", "vs_5_1");
    auto psBytecode = CompileShaderFromFile(L"shaders/sky_ps.hlsl", "main", "ps_5_1");

    if (!vsBytecode || !psBytecode)
    {
        core::Log::Error("Failed to compile shaders for sky PSO");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig.Get();
    psoDesc.VS.pShaderBytecode = vsBytecode->GetBufferPointer();
    psoDesc.VS.BytecodeLength  = vsBytecode->GetBufferSize();
    psoDesc.PS.pShaderBytecode = psBytecode->GetBufferPointer();
    psoDesc.PS.BytecodeLength  = psBytecode->GetBufferSize();

    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;
    psoDesc.RasterizerState.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    psoDesc.BlendState.AlphaToCoverageEnable  = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable           = FALSE;
    psoDesc.BlendState.RenderTarget[0].LogicOpEnable         = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable    = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = kHDRFormat;   // linear scene target, not the back buffer
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc            = { 1, 0 };
    psoDesc.SampleMask            = UINT_MAX;

    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_skyPSO));
    if (FAILED(hr))
    {
        core::Log::Errorf("CreateGraphicsPipelineState (sky) failed: 0x%08X", hr);
        return false;
    }

    m_skyPSO->SetName(L"RasterSkyPSO");
    core::Log::Info("Sky PSO created (full-screen sky, depth off)");
    return true;
}

// =============================================================================
// Constant Buffer Upload Ring — persistently mapped, 256-byte aligned
// =============================================================================
bool Renderer::CreateConstantBuffers(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = kCBRingSize;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    const wchar_t* names[] = { L"CBUpload[0]", L"CBUpload[1]", L"CBUpload[2]" };

    for (uint32_t i = 0; i < kFrameCount; i++)
    {
        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_cbUploadBuffers[i]));

        if (FAILED(hr))
        {
            core::Log::Errorf("Failed to create CB upload buffer %u: 0x%08X", i, hr);
            return false;
        }

        m_cbUploadBuffers[i]->SetName(names[i]);

        // Persistently map — never unmap during normal operation
        D3D12_RANGE readRange = { 0, 0 };
        hr = m_cbUploadBuffers[i]->Map(0, &readRange,
                                        reinterpret_cast<void**>(&m_cbMappedPtrs[i]));
        if (FAILED(hr))
        {
            core::Log::Errorf("Failed to map CB upload buffer %u: 0x%08X", i, hr);
            return false;
        }
    }

    core::Log::Infof("CB upload ring created: %u KB per frame, %u frames",
                     kCBRingSize / 1024, kFrameCount);
    return true;
}

bool Renderer::CreateTextureHeap(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = kMaxRasterTextures;
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_textureHeap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create raster texture heap: 0x%08X", hr);
        return false;
    }

    m_textureHeap->SetName(L"RasterTextureHeap");
    m_textureDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Null-fill the WHOLE table, not just slot 0. The root signature binds all
    // kMaxRasterTextures descriptors as one range, so every slot is nominally
    // part of a bound table even before any texture is registered. Leaving the
    // unallocated ones uninitialised was safe only because the range is
    // DESCRIPTORS_VOLATILE and every sample in basic_ps.hlsl is gated behind
    // useAlbedoTexture / useNormalTexture - i.e. it relied on an invariant held
    // in the shader rather than in this heap. path_tracer.cpp already null-fills
    // its equivalent ranges; the two subsystems disagreed on this, and the
    // raster side was the one depending on someone else being careful.
    for (uint32_t i = 0; i < kMaxRasterTextures; ++i)
        WriteNullTextureDescriptor(device, i);

    // Slot 0 stays the null-SRV fallback and slot 1 the shadow map, so
    // allocation starts at 2 and the allocator can never recycle either.
    m_textureAllocator.Init(kMaxRasterTextures, kShadowDescriptorIndex + 1);

    core::Log::Infof("Raster texture heap created (%u descriptors, %u usable)",
                     kMaxRasterTextures,
                     kMaxRasterTextures - (kShadowDescriptorIndex + 1));
    return true;
}

void Renderer::WriteNullTextureDescriptor(ID3D12Device* device, uint32_t descriptorIndex)
{
    if (!device || !m_textureHeap || descriptorIndex >= kMaxRasterTextures)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_textureHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_textureDescSize;
    device->CreateShaderResourceView(nullptr, &nullSrv, handle);
}

void Renderer::ReleaseTextureDescriptor(D3D12Device& device, DescriptorHandle descriptor)
{
    if (!descriptor.IsValid()) return;

    // Parked against the value signalled at the END of this frame, not the last
    // one already signalled: a command list recorded during this frame may still
    // reference the slot, and the GPU reads shader-visible descriptors at
    // execution time for the volatile ranges this engine binds.
    if (!m_textureAllocator.Release(descriptor, device.PendingFenceValue()))
    {
        core::Log::Warnf("Ignoring release of unowned raster texture descriptor %u gen=%u",
                         descriptor.index, descriptor.generation);
    }
}

void Renderer::ReclaimTextureDescriptors(D3D12Device& device)
{
    m_textureAllocator.Reclaim(
        device.CompletedFenceValue(),
        [this, d3dDevice = device.Device()](DescriptorHandle descriptor)
        {
            WriteNullTextureDescriptor(d3dDevice, descriptor.index);
        });
}

DescriptorHandle Renderer::RegisterTexture(ID3D12Device* device, const Texture& texture)
{
    if (!device || !m_textureHeap || !texture.IsValid())
        return {};

    const DescriptorHandle descriptor = m_textureAllocator.Allocate();
    if (!descriptor.IsValid())
    {
        core::Log::Errorf("Raster texture heap is full (%u in use, %u capacity)",
                          m_textureAllocator.InUse(), m_textureAllocator.Capacity());
        return {};
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_textureHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptor.index) * m_textureDescSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texture.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = texture.mipCount;
    device->CreateShaderResourceView(texture.resource.Get(), &srvDesc, handle);

    core::Log::Infof("Raster texture registered: descriptor=%u (%ux%u, mips=%u)",
                     descriptor.index, texture.width, texture.height, texture.mipCount);
    return descriptor;
}

// =============================================================================
// Upload a constant buffer sub-allocation
// =============================================================================
D3D12_GPU_VIRTUAL_ADDRESS Renderer::UploadCB(const void* data, uint32_t dataSize)
{
    uint32_t alignedSize = AlignCBSize(dataSize);

    // Check for overflow
    if (m_cbOffset + alignedSize > kCBRingSize)
    {
        core::Log::Error("CB upload ring overflow! Increase kCBRingSize.");
        return 0;
    }

    // Copy data to the mapped buffer
    uint8_t* dest = m_cbMappedPtrs[m_currentFrame] + m_cbOffset;
    memcpy(dest, data, dataSize);
    // Zero padding between data and aligned boundary
    if (alignedSize > dataSize)
        memset(dest + dataSize, 0, alignedSize - dataSize);

    // Calculate GPU virtual address
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddr =
        m_cbUploadBuffers[m_currentFrame]->GetGPUVirtualAddress() + m_cbOffset;

    m_cbOffset += alignedSize;
    if (m_cbOffset > m_cbPeak) m_cbPeak = m_cbOffset;
    return gpuAddr;
}

// =============================================================================
// BeginFrame — set up pipeline state and per-frame constants
// =============================================================================
void Renderer::BeginFrame(D3D12Device& device, const Camera& camera)
{
    m_currentFrame = device.FrameIndex();
    m_cbOffset = 0; // Reset ring for this frame

    auto* cmd = device.CmdList();
    ID3D12DescriptorHeap* heaps[] = { m_textureHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    // Set pipeline state
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Cache view-projection
    float aspect = static_cast<float>(device.Width()) / static_cast<float>(device.Height());
    m_viewProj = camera.ViewProjectionMatrix(aspect);
    m_eyePos = {};

    // Upload per-frame constants (b1)
    CBPerFrame perFrame = {};
    perFrame.lightDir[0] = m_lightDir.x;
    perFrame.lightDir[1] = m_lightDir.y;
    perFrame.lightDir[2] = m_lightDir.z;
    perFrame.lightColor[0] = m_lightColor.x;
    perFrame.lightColor[1] = m_lightColor.y;
    perFrame.lightColor[2] = m_lightColor.z;
    perFrame.ambientColor[0] = m_ambientColor.x;
    perFrame.ambientColor[1] = m_ambientColor.y;
    perFrame.ambientColor[2] = m_ambientColor.z;
    perFrame.eyePos[0] = m_eyePos.x;
    perFrame.eyePos[1] = m_eyePos.y;
    perFrame.eyePos[2] = m_eyePos.z;

    // Camera basis for world-space sky reconstruction in the pixel shader.
    // Derived the same way the path tracer does it (path_tracer.cpp) so the two
    // render paths agree on where the horizon is.
    const core::Vec3f camRight   = camera.Right();
    const core::Vec3f camForward = camera.Forward();
    const core::Vec3f camUp      = camForward.Cross(camRight).Normalized();
    perFrame.camRight[0]   = camRight.x;
    perFrame.camRight[1]   = camRight.y;
    perFrame.camRight[2]   = camRight.z;
    perFrame.camUp[0]      = camUp.x;
    perFrame.camUp[1]      = camUp.y;
    perFrame.camUp[2]      = camUp.z;
    perFrame.camForward[0] = camForward.x;
    perFrame.camForward[1] = camForward.y;
    perFrame.camForward[2] = camForward.z;
    const float fovYRad    = camera.GetFOV() * (3.14159265358979323846f / 180.0f);
    perFrame.tanHalfFovY   = std::tan(fovYRad * 0.5f);
    perFrame.aspect        = aspect;
    memcpy(perFrame.lightViewProj, m_lightViewProj.Data(), sizeof(float) * 16);

    auto perFrameAddr = UploadCB(&perFrame, sizeof(perFrame));
    cmd->SetGraphicsRootConstantBufferView(1, perFrameAddr);

    cmd->SetGraphicsRootDescriptorTable(3, m_textureHeap->GetGPUDescriptorHandleForHeapStart());

    // Shadow map SRV. Its own root parameter rather than a slot the material
    // table reaches, so material descriptor allocation and the shadow map stay
    // independent - the shadow map must exist before any material is registered.
    D3D12_GPU_DESCRIPTOR_HANDLE shadowGpu = m_textureHeap->GetGPUDescriptorHandleForHeapStart();
    shadowGpu.ptr += static_cast<UINT64>(kShadowDescriptorIndex) * m_textureDescSize;
    cmd->SetGraphicsRootDescriptorTable(4, shadowGpu);
}

void Renderer::DrawSky(D3D12Device& device)
{
    auto* cmd = device.CmdList();
    cmd->SetPipelineState(m_skyPSO.Get());
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
    cmd->SetPipelineState(m_pso.Get());
}

// =============================================================================
// DrawMesh — per-object transform + material
// =============================================================================
void Renderer::DrawMesh(D3D12Device& device, const Mesh& mesh,
                        const core::Mat4x4& worldMatrix,
                        const core::Color& albedo,
                        float roughness, float metallic,
                        const Texture* albedoTexture,
                        const Texture* normalTexture,
                        const Texture* ormTexture,
                        const Texture* emissiveTexture,
                        const core::Color& emissive,
                        float emissiveStrength)
{
    if (!mesh.IsValid()) return;

    auto* cmd = device.CmdList();
    bool useAlbedoTexture = albedoTexture &&
        albedoTexture->IsValid() &&
        m_textureAllocator.IsInUse(albedoTexture->descriptor);
    // Same liveness predicate the other two use: a generational descriptor
    // handle, not a bare index, so a recycled slot cannot pass.
    bool useOrmTexture = ormTexture &&
        ormTexture->IsValid() &&
        m_textureAllocator.IsInUse(ormTexture->descriptor);
    bool useNormalTexture = normalTexture &&
        normalTexture->IsValid() &&
        m_textureAllocator.IsInUse(normalTexture->descriptor);
    bool useEmissiveTexture = emissiveTexture &&
        emissiveTexture->IsValid() &&
        m_textureAllocator.IsInUse(emissiveTexture->descriptor);

    // Compute world-view-projection
    core::Mat4x4 wvp = worldMatrix * m_viewProj;

    // Compute world inverse transpose for correct normal transformation
    // Handles non-uniform scale correctly (not just a copy of world matrix)
    core::Mat4x4 worldInvTranspose = core::Mat4x4::InverseTranspose3x3(worldMatrix);

    // Upload per-object constants (b0)
    CBPerObject perObject = {};
    memcpy(perObject.worldViewProj, wvp.Data(), sizeof(float) * 16);
    memcpy(perObject.world, worldMatrix.Data(), sizeof(float) * 16);
    memcpy(perObject.worldInvTranspose, worldInvTranspose.Data(), sizeof(float) * 16);

    auto perObjectAddr = UploadCB(&perObject, sizeof(perObject));
    cmd->SetGraphicsRootConstantBufferView(0, perObjectAddr);

    // Upload material constants (b2)
    CBMaterial material = {};
    material.albedo[0] = albedo.r;
    material.albedo[1] = albedo.g;
    material.albedo[2] = albedo.b;
    material.albedo[3] = albedo.a;
    material.roughness = roughness;
    material.metallic  = metallic;
    material.useAlbedoTexture = useAlbedoTexture ? 1u : 0u;
    material.useNormalTexture = useNormalTexture ? 1u : 0u;
    material.albedoTextureIndex = useAlbedoTexture ? albedoTexture->descriptor.index : 0u;
    material.normalTextureIndex = useNormalTexture ? normalTexture->descriptor.index : 0u;
    material.useOrmTexture = useOrmTexture ? 1u : 0u;
    material.ormTextureIndex = useOrmTexture ? ormTexture->descriptor.index : 0u;
    material.emissive[0] = emissive.r;
    material.emissive[1] = emissive.g;
    material.emissive[2] = emissive.b;
    material.emissiveStrength = emissiveStrength;
    material.useEmissiveTexture = useEmissiveTexture ? 1u : 0u;
    material.emissiveTextureIndex =
        useEmissiveTexture ? emissiveTexture->descriptor.index : 0u;

    auto materialAddr = UploadCB(&material, sizeof(material));
    cmd->SetGraphicsRootConstantBufferView(2, materialAddr);

    // Bind geometry and draw
    cmd->IASetVertexBuffers(0, 1, &mesh.vbView);
    cmd->IASetIndexBuffer(&mesh.ibView);
    cmd->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
}

// =============================================================================
// SetDirectionalLight
// =============================================================================
void Renderer::SetDirectionalLight(const core::Vec3f& direction,
                                    const core::Vec3f& color,
                                    const core::Vec3f& ambient)
{
    m_lightDir = direction.Normalized();
    m_lightColor = color;
    m_ambientColor = ambient;
    UpdateLightMatrix();
}

// =============================================================================
// Shadow map
// =============================================================================

void Renderer::UpdateLightMatrix()
{
    // Everything the raster path draws is already camera-relative, so the camera
    // sits at the origin of the space these matrices operate in. The light
    // frustum is therefore centred on the origin and the map follows the viewer
    // for free - no camera position enters this calculation, and none should.
    //
    // m_lightDir points TOWARD the light, so the light is up that direction and
    // looks back down it at the origin.
    const core::Vec3f eye = m_lightDir * (kShadowDepthRange * 0.5f);

    // Any up vector works except one parallel to the view direction, which would
    // make the cross product in LookAt degenerate.
    core::Vec3f up = core::Vec3f(0.0f, 1.0f, 0.0f);
    if (std::fabs(m_lightDir.y) > 0.99f)
        up = core::Vec3f(0.0f, 0.0f, 1.0f);

    const core::Mat4x4 lightView =
        core::Mat4x4::LookAt(eye, core::Vec3f(0.0f, 0.0f, 0.0f), up);
    const core::Mat4x4 lightProj =
        core::Mat4x4::OrthoLH(kShadowExtent * 2.0f, kShadowExtent * 2.0f,
                              0.1f, kShadowDepthRange);

    m_lightViewProj = lightView * lightProj;
}

bool Renderer::CreateShadowResources(ID3D12Device* device)
{
    // R32_TYPELESS so one resource can carry both a D32_FLOAT DSV (shadow pass)
    // and an R32_FLOAT SRV (material pass). A D32_FLOAT resource cannot be given
    // a shader-resource view directly.
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = kShadowMapSize;
    desc.Height           = kShadowMapSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format               = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth   = 1.0f;
    clear.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
        IID_PPV_ARGS(&m_shadowMap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create shadow map: 0x%08X", hr);
        return false;
    }
    m_shadowMap->SetName(L"ShadowMap");
    m_shadowIsDepthTarget = false;

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hr = device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_shadowDsvHeap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create shadow DSV heap: 0x%08X", hr);
        m_shadowMap.Reset();
        return false;
    }
    m_shadowDsvHeap->SetName(L"ShadowDsvHeap");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format        = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_shadowMap.Get(), &dsvDesc,
        m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart());

    // SRV into the reserved slot of the shared material heap. Only one
    // shader-visible CBV_SRV_UAV heap can be bound at a time, so anything the
    // material pass samples has to live here. The allocator was initialised to
    // start past this index, so nothing else can claim it.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_textureHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(kShadowDescriptorIndex) * m_textureDescSize;
    device->CreateShaderResourceView(m_shadowMap.Get(), &srvDesc, srvCpu);

    UpdateLightMatrix();

    // Structured marker rather than prose: the smoke harness asserts on this,
    // so rewording the human-readable line above cannot silently disarm it.
    core::Log::Infof("[SMOKE] shadow_map=ok shadow_map_size=%u shadow_map_slot=%u",
                     kShadowMapSize, kShadowDescriptorIndex);
    core::Log::Infof("Shadow map created (%ux%u D32_FLOAT, descriptor slot %u)",
                     kShadowMapSize, kShadowMapSize, kShadowDescriptorIndex);
    return true;
}

bool Renderer::CreateShadowPSO(ID3D12Device* device)
{
    auto vsBytecode = CompileShaderFromFile(L"shaders/shadow_vs.hlsl", "main", "vs_5_1");
    if (!vsBytecode)
    {
        core::Log::Error("Failed to compile shadow vertex shader");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig.Get();
    psoDesc.VS.pShaderBytecode = vsBytecode->GetBufferPointer();
    psoDesc.VS.BytecodeLength  = vsBytecode->GetBufferSize();
    // No pixel shader: depth only.
    psoDesc.InputLayout.pInputElementDescs = kVertexLayout;
    psoDesc.InputLayout.NumElements        = kVertexLayoutCount;

    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    // Constant and slope-scaled depth bias push stored depth away from the
    // light, so a surface does not shadow itself where the shadow-map texel and
    // the shaded pixel disagree by less than a texel of depth. Slope scaling
    // matters more than the constant term: the error grows with the angle
    // between surface and light, which is exactly what acne is.
    psoDesc.RasterizerState.DepthBias             = 2000;
    psoDesc.RasterizerState.DepthBiasClamp        = 0.0f;
    psoDesc.RasterizerState.SlopeScaledDepthBias  = 2.5f;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets      = 0;
    psoDesc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc            = { 1, 0 };
    psoDesc.SampleMask            = UINT_MAX;

    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_shadowPSO));
    if (FAILED(hr))
    {
        core::Log::Errorf("CreateGraphicsPipelineState (shadow) failed: 0x%08X", hr);
        return false;
    }
    m_shadowPSO->SetName(L"ShadowDepthPSO");
    core::Log::Info("Shadow depth PSO created (VS only, no render targets)");
    return true;
}

void Renderer::BeginShadowPass(D3D12Device& device)
{
    if (!m_shadowMap || !m_shadowPSO) return;

    auto* cmd = device.CmdList();

    // Rebuild every frame: the light can move, and the frustum is anchored to
    // the camera, which certainly does.
    UpdateLightMatrix();

    if (!m_shadowIsDepthTarget)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = m_shadowMap.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &barrier);
        m_shadowIsDepthTarget = true;
    }

    auto dsv = m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f,
                          static_cast<float>(kShadowMapSize),
                          static_cast<float>(kShadowMapSize), 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0,
                      static_cast<LONG>(kShadowMapSize),
                      static_cast<LONG>(kShadowMapSize) };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_shadowPSO.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::EndShadowPass(D3D12Device& device)
{
    if (!m_shadowMap || !m_shadowIsDepthTarget) return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_shadowMap.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    device.CmdList()->ResourceBarrier(1, &barrier);
    m_shadowIsDepthTarget = false;
}

bool Renderer::RecordShadowMapReadback(D3D12Device& device)
{
    if (!m_shadowMap) return false;
    auto* cmd = device.CmdList();
    auto* dev = device.Device();
    if (!cmd || !dev) return false;

    // The copy destination describes the REGION, not the resource: a placed
    // footprint whose row pitch is 256-byte aligned as the API requires.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    footprint.Offset                    = 0;
    footprint.Footprint.Format          = DXGI_FORMAT_R32_FLOAT;
    footprint.Footprint.Width           = kShadowProbeSize;
    footprint.Footprint.Height          = kShadowProbeSize;
    footprint.Footprint.Depth           = 1;
    footprint.Footprint.RowPitch        =
        (kShadowProbeSize * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);

    const UINT64 totalBytes =
        static_cast<UINT64>(footprint.Footprint.RowPitch) * kShadowProbeSize;

    if (!m_shadowReadback)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width            = totalBytes;
        bufDesc.Height           = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels        = 1;
        bufDesc.Format           = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc       = { 1, 0 };
        bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_shadowReadback));
        if (FAILED(hr))
        {
            core::Log::Errorf("Shadow map readback allocation failed: 0x%08X", hr);
            return false;
        }
        m_shadowReadback->SetName(L"ShadowMapReadback");
    }

    // EndShadowPass left the map in PIXEL_SHADER_RESOURCE; hand it back that way.
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_shadowMap.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource       = m_shadowReadback.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource        = m_shadowMap.Get();
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    const UINT half = kShadowProbeSize / 2u;
    D3D12_BOX box = {};
    box.left   = kShadowMapSize / 2u - half;
    box.top    = kShadowMapSize / 2u - half;
    box.front  = 0;
    box.right  = kShadowMapSize / 2u + half;
    box.bottom = kShadowMapSize / 2u + half;
    box.back   = 1;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &barrier);
    return true;
}

bool Renderer::ReadShadowMapCoverage(float& writtenFraction, float& minDepth) const
{
    writtenFraction = 0.0f;
    minDepth        = 1.0f;
    if (!m_shadowReadback) return false;

    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 };  // size filled below
    const UINT rowPitch =
        (kShadowProbeSize * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    readRange.End = static_cast<SIZE_T>(rowPitch) * kShadowProbeSize;

    HRESULT hr = m_shadowReadback->Map(0, &readRange, &mapped);
    if (FAILED(hr) || !mapped)
    {
        core::Log::Errorf("Shadow map readback Map failed: 0x%08X", hr);
        return false;
    }

    uint64_t written = 0;
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < kShadowProbeSize; ++y)
    {
        const auto* row = reinterpret_cast<const float*>(bytes + static_cast<size_t>(y) * rowPitch);
        for (uint32_t x = 0; x < kShadowProbeSize; ++x)
        {
            const float d = row[x];
            // Strictly less than the clear value. A depth of exactly 1.0 is
            // either the cleared background or geometry at the far plane, and
            // neither counts as evidence the pass drew anything.
            if (d < 1.0f)
            {
                ++written;
                if (d < minDepth) minDepth = d;
            }
        }
    }

    // Nothing was written back, so tell the runtime not to flush anything.
    const D3D12_RANGE noWrite = { 0, 0 };
    m_shadowReadback->Unmap(0, &noWrite);

    writtenFraction = static_cast<float>(written) /
                      static_cast<float>(kShadowProbeSize * kShadowProbeSize);
    return true;
}

void Renderer::DrawMeshShadow(D3D12Device& device, const Mesh& mesh,
                              const core::Mat4x4& worldMatrix)
{
    if (!mesh.IsValid() || !m_shadowMap) return;

    auto* cmd = device.CmdList();

    // The same CBPerObject the main pass uses, with the light matrix
    // substituted for the camera one. world and worldInvTranspose are filled in
    // even though shadow_vs.hlsl ignores them: the struct is shared, and leaving
    // stale bytes in a mapped upload ring is how a later shader change turns
    // into a heisenbug.
    CBPerObject perObject = {};
    const core::Mat4x4 wvp = worldMatrix * m_lightViewProj;
    memcpy(perObject.worldViewProj, wvp.Data(), sizeof(float) * 16);
    memcpy(perObject.world, worldMatrix.Data(), sizeof(float) * 16);
    const core::Mat4x4 worldInvTranspose = core::Mat4x4::InverseTranspose3x3(worldMatrix);
    memcpy(perObject.worldInvTranspose, worldInvTranspose.Data(), sizeof(float) * 16);

    cmd->SetGraphicsRootConstantBufferView(0, UploadCB(&perObject, sizeof(perObject)));
    cmd->IASetVertexBuffers(0, 1, &mesh.vbView);
    cmd->IASetIndexBuffer(&mesh.ibView);
    cmd->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
}

} // namespace render
