// =============================================================================
// render/renderer.cpp — Renderer Implementation
// =============================================================================

#include "renderer.h"
#include "shader_utils.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>
#include <cstdint>

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
    m_nextTextureDescriptor = 1;
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
        srvDesc.NumDescriptors = 1;
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

    m_hdrWidth  = width;
    m_hdrHeight = height;
    core::Log::Infof("HDR scene target created: %ux%u (R16G16B16A16_FLOAT)", width, height);
    return true;
}

bool Renderer::ResizeHDRTarget(D3D12Device& device, uint32_t width, uint32_t height)
{
    if (width == m_hdrWidth && height == m_hdrHeight && m_hdrTarget) return true;

    // The outgoing target may still be referenced by frames in flight, so it goes
    // through the fence-guarded queue rather than being dropped here.
    if (m_hdrTarget)
        device.DeferredRelease(m_hdrTarget);

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
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParam.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters     = 1;
    rsDesc.pParameters       = &rootParam;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &sampler;
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

    // The back buffer becomes a render target only now, for the resolve itself.
    device.TransitionResource(device.CurrentBackBuffer(),
                              D3D12_RESOURCE_STATE_PRESENT,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);

    auto rtv = device.CurrentRTV();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);   // no depth: full overwrite

    ID3D12DescriptorHeap* heaps[] = { m_hdrSrvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_tonemapRootSig.Get());
    cmd->SetGraphicsRootDescriptorTable(0, m_hdrSrvHeap->GetGPUDescriptorHandleForHeapStart());
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
//   Slot 3: Descriptor table for material textures (t0-t127)
//   Static sampler at s0
// Total: 7 DWORDs - well within 64 DWORD limit
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
    staticSampler.RegisterSpace    = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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

        D3D12_ROOT_PARAMETER1 rootParams[4] = {};

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

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC vrsDesc = {};
        vrsDesc.Version                    = D3D_ROOT_SIGNATURE_VERSION_1_1;
        vrsDesc.Desc_1_1.NumParameters     = _countof(rootParams);
        vrsDesc.Desc_1_1.pParameters       = rootParams;
        vrsDesc.Desc_1_1.NumStaticSamplers = 1;
        vrsDesc.Desc_1_1.pStaticSamplers   = &staticSampler;
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

        D3D12_ROOT_PARAMETER rootParams[4] = {};

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

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters     = _countof(rootParams);
        rsDesc.pParameters       = rootParams;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers   = &staticSampler;
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
    core::Log::Infof("Root signature created (v%s, 3 root CBVs + raster texture table + 1 static sampler, 7 DWORDs)",
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
    auto psBytecode = CompileShaderFromFile(L"shaders/basic_ps.hlsl", "main", "ps_5_1");

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

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
    nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &nullSrv, m_textureHeap->GetCPUDescriptorHandleForHeapStart());

    core::Log::Infof("Raster texture heap created (%u descriptors)", kMaxRasterTextures);
    return true;
}

uint32_t Renderer::RegisterTexture(ID3D12Device* device, const Texture& texture)
{
    if (!device || !m_textureHeap || !texture.IsValid())
        return UINT32_MAX;

    if (m_nextTextureDescriptor >= kMaxRasterTextures)
    {
        core::Log::Error("Raster texture heap is full");
        return UINT32_MAX;
    }

    uint32_t descriptorIndex = m_nextTextureDescriptor++;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_textureHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_textureDescSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texture.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = texture.mipCount;
    device->CreateShaderResourceView(texture.resource.Get(), &srvDesc, handle);

    core::Log::Infof("Raster texture registered: descriptor=%u (%ux%u, mips=%u)",
                     descriptorIndex, texture.width, texture.height, texture.mipCount);
    return descriptorIndex;
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

    auto perFrameAddr = UploadCB(&perFrame, sizeof(perFrame));
    cmd->SetGraphicsRootConstantBufferView(1, perFrameAddr);

    cmd->SetGraphicsRootDescriptorTable(3, m_textureHeap->GetGPUDescriptorHandleForHeapStart());
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
                        const Texture* normalTexture)
{
    if (!mesh.IsValid()) return;

    auto* cmd = device.CmdList();
    bool useAlbedoTexture = albedoTexture &&
        albedoTexture->IsValid() &&
        albedoTexture->descriptorIndex != UINT32_MAX &&
        albedoTexture->descriptorIndex < m_nextTextureDescriptor;
    bool useNormalTexture = normalTexture &&
        normalTexture->IsValid() &&
        normalTexture->descriptorIndex != UINT32_MAX &&
        normalTexture->descriptorIndex < m_nextTextureDescriptor;

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
    material.albedoTextureIndex = useAlbedoTexture ? albedoTexture->descriptorIndex : 0u;
    material.normalTextureIndex = useNormalTexture ? normalTexture->descriptorIndex : 0u;

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
}

} // namespace render
