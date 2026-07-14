// =============================================================================
// render/renderer.cpp — Renderer Implementation
// =============================================================================

#include "renderer.h"
#include "shader_utils.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>

namespace render
{

// =============================================================================
// Init
// =============================================================================
bool Renderer::Init(D3D12Device& device)
{
    if (!CreateRootSignature(device.Device())) return false;
    if (!CreatePSO(device.Device()))           return false;
    if (!CreateConstantBuffers(device.Device())) return false;

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
    core::Log::Info("Renderer shut down");
}

// =============================================================================
// Root Signature — v1.1 with fallback to v1.0
// =============================================================================
// Layout (research-informed):
//   Slot 0: Root CBV at b0 (per-object) — 2 DWORDs, hot, changes every draw
//   Slot 1: Root CBV at b1 (per-frame)  — 2 DWORDs, warm, changes once/frame
//   Slot 2: Root CBV at b2 (material)   — 2 DWORDs, warm, changes per material
//   Static sampler at s0
// Total: 6 DWORDs — well within 64 DWORD limit
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
        D3D12_ROOT_PARAMETER1 rootParams[3] = {};

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
        D3D12_ROOT_PARAMETER rootParams[3] = {};

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
    core::Log::Infof("Root signature created (v%s, 3 root CBVs + 1 static sampler, 6 DWORDs)",
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
    psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
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

    // Set pipeline state
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Cache view-projection
    float aspect = static_cast<float>(device.Width()) / static_cast<float>(device.Height());
    m_viewProj = camera.ViewProjectionMatrix(aspect);
    m_eyePos = camera.Position();

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

    auto perFrameAddr = UploadCB(&perFrame, sizeof(perFrame));
    cmd->SetGraphicsRootConstantBufferView(1, perFrameAddr);
}

// =============================================================================
// DrawMesh — per-object transform + material
// =============================================================================
void Renderer::DrawMesh(D3D12Device& device, const Mesh& mesh,
                        const core::Mat4x4& worldMatrix,
                        const core::Color& albedo,
                        float roughness, float metallic)
{
    if (!mesh.IsValid()) return;

    auto* cmd = device.CmdList();

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
