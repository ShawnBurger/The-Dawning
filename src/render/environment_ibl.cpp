#include "environment_ibl.h"

#include <cmath>
#include <cstring>

#include "d3d12_device.h"
#include "shader_utils.h"
#include "../core/log.h"
#include "../core/sky_radiance.h"

using Microsoft::WRL::ComPtr;

namespace render
{
namespace
{

// -----------------------------------------------------------------------------
// IEEE 754 binary16 -> binary32.
// -----------------------------------------------------------------------------
// The cube is R16G16B16A16_FLOAT (it matches Renderer::kHDRFormat), so the
// readback has to decode halves. Written out rather than pulled from a library
// because this translation unit already links no maths dependency and the
// subnormal branch is the only part with any subtlety.
float HalfToFloat(uint16_t h)
{
    const uint32_t sign =  static_cast<uint32_t>(h >> 15) & 0x1u;
    uint32_t       exp  =  static_cast<uint32_t>(h >> 10) & 0x1Fu;
    uint32_t       mant =  static_cast<uint32_t>(h)       & 0x3FFu;

    uint32_t bits;
    if (exp == 0u)
    {
        if (mant == 0u)
        {
            bits = sign << 31;                       // +/- zero
        }
        else
        {
            // Subnormal: renormalise into a binary32 normal.
            exp = 127u - 15u + 1u;
            while ((mant & 0x400u) == 0u)
            {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = (sign << 31) | (exp << 23) | (mant << 13);
        }
    }
    else if (exp == 31u)
    {
        bits = (sign << 31) | 0x7F800000u | (mant << 13);   // Inf / NaN
    }
    else
    {
        bits = (sign << 31) | ((exp - 15u + 127u) << 23) | (mant << 13);
    }

    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

// The state the cube rests in between bakes.
//
// BOTH bits, not PIXEL_SHADER_RESOURCE alone. Stage 4 has the DXR path sampling
// this cube, and ray-tracing shaders are NON-pixel: leaving out
// NON_PIXEL_SHADER_RESOURCE is a debug-layer error the moment the path tracer
// touches it, and it would surface a stage after the one that caused it.
constexpr D3D12_RESOURCE_STATES kCubeShaderReadState =
    static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

} // namespace

// =============================================================================
// Probe direction set
// =============================================================================
// ONE generator, used by both sides. The GPU gets these in a constant buffer and
// the CPU compares against the identical array, so assertions 1.2 and 1.3 cannot
// disagree about which directions they are talking about. Deliberately contains
// no knowledge of the cube face table - a query set derived from the face basis
// would be a second copy of the thing under test.
//
// Slots 0..5 are the six axis directions, which land dead centre on each face
// and are the sharpest possible test of a face permutation. The remainder is a
// spherical Fibonacci spiral, which covers the whole sphere evenly and therefore
// every face and both signs of every axis.
void BuildEnvironmentProbeDirections(core::Vec3f* out, uint32_t count)
{
    if (!out || count == 0)
        return;

    const core::Vec3f axes[6] = {
        {  1.0f,  0.0f,  0.0f },
        { -1.0f,  0.0f,  0.0f },
        {  0.0f,  1.0f,  0.0f },
        {  0.0f, -1.0f,  0.0f },
        {  0.0f,  0.0f,  1.0f },
        {  0.0f,  0.0f, -1.0f },
    };

    const uint32_t axisCount = (count < 6u) ? count : 6u;
    for (uint32_t i = 0; i < axisCount; ++i)
        out[i] = axes[i];

    const uint32_t spiralCount = count - axisCount;
    // Golden angle. pi * (3 - sqrt(5)).
    const float golden = 3.14159265358979323846f * (3.0f - std::sqrt(5.0f));
    for (uint32_t i = 0; i < spiralCount; ++i)
    {
        const float t     = (static_cast<float>(i) + 0.5f) / static_cast<float>(spiralCount);
        const float y     = 1.0f - 2.0f * t;
        const float r     = std::sqrt((std::max)(0.0f, 1.0f - y * y));
        const float theta = golden * static_cast<float>(i);
        out[axisCount + i] = core::Vec3f{ r * std::cos(theta), y, r * std::sin(theta) }.Normalized();
    }
}

// =============================================================================
// Init
// =============================================================================
bool EnvironmentIBL::Init(ID3D12Device* device)
{
    if (!device)
        return false;

    m_rtvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    BuildEnvironmentProbeDirections(m_probeDirections, kEnvProbeCount);

    if (!CreateCube(device))                   return false;
    if (!CreateRootSignature(device))          return false;
    if (!CreatePipelines(device))              return false;
    if (!CreateVerificationResources(device))  return false;

    return true;
}

bool EnvironmentIBL::CreateCube(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = kEnvCubeSize;
    desc.Height           = kEnvCubeSize;
    desc.DepthOrArraySize = 6;
    desc.MipLevels        = static_cast<UINT16>(kEnvCubeMips);
    desc.Format           = kEnvCubeFormat;
    desc.SampleDesc       = { 1, 0 };
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kEnvCubeFormat;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        kCubeShaderReadState, &clear, IID_PPV_ARGS(&m_cube));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create environment cubemap: 0x%08X", hr);
        return false;
    }
    m_cube->SetName(L"EnvironmentCube");

    // 48 RTVs: one per (face, mip). Non-shader-visible, so this heap never
    // competes with the 128-descriptor material heap.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kEnvCubeMips * 6;
    rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_cubeRtvHeap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create environment cube RTV heap: 0x%08X", hr);
        return false;
    }
    m_cubeRtvHeap->SetName(L"EnvironmentCubeRTVHeap");

    D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = m_cubeRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t mip = 0; mip < kEnvCubeMips; ++mip)
        {
            D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
            rtv.Format                         = kEnvCubeFormat;
            rtv.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv.Texture2DArray.MipSlice        = mip;
            rtv.Texture2DArray.FirstArraySlice = face;
            rtv.Texture2DArray.ArraySize       = 1;

            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvStart;
            handle.ptr += static_cast<SIZE_T>(face * kEnvCubeMips + mip) * m_rtvDescSize;
            device->CreateRenderTargetView(m_cube.Get(), &rtv, handle);
        }
    }

    // Readback for assertions 1.4 and 1.5. Subresource index is
    // mip + face * mipCount, which is the order GetCopyableFootprints uses.
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&desc, 0, kEnvCubeMips * 6, 0,
                                  m_cubeFootprints, nullptr, nullptr, &totalBytes);
    m_cubeReadbackBytes = totalBytes;

    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = totalBytes;
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc       = { 1, 0 };
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = device->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_cubeReadback));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create environment cube readback: 0x%08X", hr);
        return false;
    }
    m_cubeReadback->SetName(L"EnvironmentCubeReadback");
    return true;
}

bool EnvironmentIBL::CreateRootSignature(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_RANGE directionRange = {};
    directionRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    directionRange.NumDescriptors                    = 1;
    directionRange.BaseShaderRegister                = 0;
    directionRange.RegisterSpace                     = 0;
    directionRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};

    // b0: { faceIndex, roughness, sampleCount, invFaceSize }
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace  = 0;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    // b1: the probe direction set (verification passes only).
    params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].Descriptor.RegisterSpace  = 0;
    params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // t0: the direction cubemap (verification only).
    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &directionRange;
    params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Trilinear + CLAMP. Anisotropic/WRAP - what the material sampler uses - is
    // wrong for a cube: WRAP is not the cube addressing mode and anisotropy
    // across faces means nothing here.
    //
    // Every field is assigned BEFORE the descriptor is handed to the root
    // signature desc. renderer.cpp:851-859 records a shipped dead-store bug of
    // exactly that shape in this codebase.
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias       = 0.0f;
    sampler.MaxAnisotropy    = 1;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD           = 0.0f;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.RegisterSpace    = 0;
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
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob)
            core::Log::Errorf("Environment IBL root signature error: %s",
                              static_cast<const char*>(errBlob->GetBufferPointer()));
        return false;
    }

    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr))
    {
        core::Log::Errorf("Environment IBL CreateRootSignature failed: 0x%08X", hr);
        return false;
    }
    m_rootSignature->SetName(L"EnvironmentIBLRootSignature");
    return true;
}

bool EnvironmentIBL::CreatePipelines(ID3D12Device* device)
{
    // Reuses the bloom fullscreen-triangle VS. Its output is exactly
    // { SV_POSITION, TEXCOORD0 }, which is what the IBL pixel shaders declare.
    auto vs = CompileShaderFromFile(L"shaders/bloom_vs.hlsl", "main", "vs_5_1");

    // Compiling these under FXC ps_5_1 /WX at startup is itself evidence: it is
    // what proves the shared IBL headers stay inside SM 5.1 as the raster path
    // requires.
    auto prefilterPs = CompileShaderFromFile(L"shaders/ibl_prefilter_ps.hlsl",
                                             "PrefilterPS", "ps_5_1");
    auto directionPs = CompileShaderFromFile(L"shaders/ibl_prefilter_ps.hlsl",
                                             "DirectionPS", "ps_5_1");
    auto skyProbePs  = CompileShaderFromFile(L"shaders/ibl_probe_ps.hlsl",
                                             "SkyAgreementPS", "ps_5_1");
    auto dirProbePs  = CompileShaderFromFile(L"shaders/ibl_probe_ps.hlsl",
                                             "DirectionRoundTripPS", "ps_5_1");

    if (!vs || !prefilterPs || !directionPs || !skyProbePs || !dirProbePs)
    {
        core::Log::Error("Failed to compile environment IBL shaders");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_rootSignature.Get();
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
    pso.SampleDesc       = { 1, 0 };
    pso.SampleMask       = UINT_MAX;

    struct PipelineSpec
    {
        ID3DBlob*                    ps;
        DXGI_FORMAT                  rtvFormat;
        ComPtr<ID3D12PipelineState>* target;
        const wchar_t*               name;
    };

    const PipelineSpec specs[] = {
        { prefilterPs.Get(), kEnvCubeFormat,                    &m_psoPrefilter,      L"EnvironmentPrefilterPSO" },
        { directionPs.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT,    &m_psoDirection,      L"EnvironmentDirectionPSO" },
        { skyProbePs.Get(),  DXGI_FORMAT_R32G32B32A32_FLOAT,    &m_psoSkyProbe,       L"EnvironmentSkyProbePSO" },
        { dirProbePs.Get(),  DXGI_FORMAT_R32G32B32A32_FLOAT,    &m_psoDirectionProbe, L"EnvironmentDirProbePSO" },
    };

    for (const PipelineSpec& spec : specs)
    {
        pso.PS.pShaderBytecode = spec.ps->GetBufferPointer();
        pso.PS.BytecodeLength  = spec.ps->GetBufferSize();
        pso.RTVFormats[0]      = spec.rtvFormat;

        HRESULT hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(spec.target->ReleaseAndGetAddressOf()));
        if (FAILED(hr))
        {
            core::Log::Errorf("Environment IBL PSO creation failed: 0x%08X", hr);
            return false;
        }
        (*spec.target)->SetName(spec.name);
    }

    return true;
}

bool EnvironmentIBL::CreateVerificationResources(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // --- direction cubemap ---------------------------------------------------
    D3D12_RESOURCE_DESC dirDesc = {};
    dirDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dirDesc.Width            = kEnvDirectionCubeSize;
    dirDesc.Height           = kEnvDirectionCubeSize;
    dirDesc.DepthOrArraySize = 6;
    dirDesc.MipLevels        = 1;
    dirDesc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dirDesc.SampleDesc       = { 1, 0 };
    dirDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE dirClear = {};
    dirClear.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &dirDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &dirClear, IID_PPV_ARGS(&m_directionCube));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create IBL direction cube: 0x%08X", hr);
        return false;
    }
    m_directionCube->SetName(L"EnvironmentDirectionCube");

    D3D12_DESCRIPTOR_HEAP_DESC dirRtvDesc = {};
    dirRtvDesc.NumDescriptors = 6;
    dirRtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = device->CreateDescriptorHeap(&dirRtvDesc, IID_PPV_ARGS(&m_directionRtvHeap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create IBL direction RTV heap: 0x%08X", hr);
        return false;
    }
    m_directionRtvHeap->SetName(L"EnvironmentDirectionRTVHeap");

    D3D12_CPU_DESCRIPTOR_HANDLE dirRtvStart =
        m_directionRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t face = 0; face < 6; ++face)
    {
        D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format                         = DXGI_FORMAT_R32G32B32A32_FLOAT;
        rtv.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv.Texture2DArray.MipSlice        = 0;
        rtv.Texture2DArray.FirstArraySlice = face;
        rtv.Texture2DArray.ArraySize       = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = dirRtvStart;
        handle.ptr += static_cast<SIZE_T>(face) * m_rtvDescSize;
        device->CreateRenderTargetView(m_directionCube.Get(), &rtv, handle);
    }

    // Shader-visible SRV for the direction cube, in its own one-descriptor heap.
    D3D12_DESCRIPTOR_HEAP_DESC probeHeapDesc = {};
    probeHeapDesc.NumDescriptors = 1;
    probeHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    probeHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&probeHeapDesc, IID_PPV_ARGS(&m_probeSrvHeap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create IBL probe SRV heap: 0x%08X", hr);
        return false;
    }
    m_probeSrvHeap->SetName(L"EnvironmentProbeSRVHeap");

    D3D12_SHADER_RESOURCE_VIEW_DESC dirSrv = {};
    dirSrv.Format                          = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dirSrv.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBE;
    dirSrv.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dirSrv.TextureCube.MostDetailedMip     = 0;
    dirSrv.TextureCube.MipLevels           = 1;
    dirSrv.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(m_directionCube.Get(), &dirSrv,
                                     m_probeSrvHeap->GetCPUDescriptorHandleForHeapStart());

    // --- 64x1 probe target ---------------------------------------------------
    D3D12_RESOURCE_DESC probeDesc = {};
    probeDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    probeDesc.Width            = kEnvProbeCount;
    probeDesc.Height           = 1;
    probeDesc.DepthOrArraySize = 1;
    probeDesc.MipLevels        = 1;
    probeDesc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
    probeDesc.SampleDesc       = { 1, 0 };
    probeDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // POISON, not black. The probe target is cleared to w = -1 before each draw
    // and the shaders write w = +1, so the CPU can count how many slots were
    // actually written. Clearing to zero would make "nothing ran" and "it ran
    // and produced zero" indistinguishable, which is the vacuity failure the
    // draw-probe work already paid for once.
    D3D12_CLEAR_VALUE probeClear = {};
    probeClear.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    probeClear.Color[0] = -1.0f;
    probeClear.Color[1] = -1.0f;
    probeClear.Color[2] = -1.0f;
    probeClear.Color[3] = -1.0f;

    hr = device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &probeDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &probeClear, IID_PPV_ARGS(&m_probeTarget));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create IBL probe target: 0x%08X", hr);
        return false;
    }
    m_probeTarget->SetName(L"EnvironmentProbeTarget");

    D3D12_DESCRIPTOR_HEAP_DESC probeRtvDesc = {};
    probeRtvDesc.NumDescriptors = 1;
    probeRtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = device->CreateDescriptorHeap(&probeRtvDesc, IID_PPV_ARGS(&m_probeRtvHeap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create IBL probe RTV heap: 0x%08X", hr);
        return false;
    }
    m_probeRtvHeap->SetName(L"EnvironmentProbeRTVHeap");
    device->CreateRenderTargetView(m_probeTarget.Get(), nullptr,
                                   m_probeRtvHeap->GetCPUDescriptorHandleForHeapStart());

    // Two 64-texel rows: [0] sky agreement, [1] direction round trip. 64 * 16 B
    // is 1024, already a multiple of both the 256-byte row pitch alignment and
    // the 512-byte placed-footprint alignment.
    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC probeReadbackDesc = {};
    probeReadbackDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    probeReadbackDesc.Width            = 2ull * kEnvProbeCount * 16ull;
    probeReadbackDesc.Height           = 1;
    probeReadbackDesc.DepthOrArraySize = 1;
    probeReadbackDesc.MipLevels        = 1;
    probeReadbackDesc.Format           = DXGI_FORMAT_UNKNOWN;
    probeReadbackDesc.SampleDesc       = { 1, 0 };
    probeReadbackDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = device->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &probeReadbackDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_probeReadback));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create IBL probe readback: 0x%08X", hr);
        return false;
    }
    m_probeReadback->SetName(L"EnvironmentProbeReadback");

    // --- probe direction constant buffer -------------------------------------
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc = probeReadbackDesc;
    cbDesc.Width = kEnvProbeCount * 16ull;

    hr = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_probeDirectionCB));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create IBL probe direction CB: 0x%08X", hr);
        return false;
    }
    m_probeDirectionCB->SetName(L"EnvironmentProbeDirectionCB");

    float* mapped = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    hr = m_probeDirectionCB->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to map IBL probe direction CB: 0x%08X", hr);
        return false;
    }
    for (uint32_t i = 0; i < kEnvProbeCount; ++i)
    {
        mapped[i * 4 + 0] = m_probeDirections[i].x;
        mapped[i * 4 + 1] = m_probeDirections[i].y;
        mapped[i * 4 + 2] = m_probeDirections[i].z;
        mapped[i * 4 + 3] = 0.0f;
    }
    m_probeDirectionCB->Unmap(0, nullptr);

    return true;
}

// =============================================================================
// Bake + verify
// =============================================================================
bool EnvironmentIBL::EnsureBuilt(D3D12Device& device, uint32_t skyRevision)
{
    // The revision early-out. A counter rather than a bool so a later day/night
    // cycle is an edit rather than a redesign (IBL_DESIGN.md section 5).
    if (m_bakedRevision == skyRevision)
        return true;

    if (!m_cube || !m_psoPrefilter)
        return false;

    if (!device.WaitForCurrentFrame() || !device.ResetCommandList())
    {
        core::Log::Error("Unable to begin environment IBL command list");
        return false;
    }

    RecordBake(device);
    RecordVerification(device);

    const HRESULT closeHr = device.CmdList()->Close();
    if (FAILED(closeHr))
    {
        core::Log::Errorf("Environment IBL command list Close failed: 0x%08X", closeHr);
        return false;
    }

    ID3D12CommandList* lists[] = { device.CmdList() };
    device.CmdQueue()->ExecuteCommandLists(1, lists);
    if (!device.WaitForGpu())
        return false;

    m_bakedRevision = skyRevision;

    return ReadbackAndValidate();
}

void EnvironmentIBL::RecordBake(D3D12Device& device)
{
    ID3D12GraphicsCommandList* cmd = device.CmdList();

    D3D12_RESOURCE_BARRIER toTarget =
        Transition(m_cube.Get(), kCubeShaderReadState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &toTarget);

    cmd->SetGraphicsRootSignature(m_rootSignature.Get());
    cmd->SetPipelineState(m_psoPrefilter.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = m_cubeRtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t mip = 0; mip < kEnvCubeMips; ++mip)
        {
            const uint32_t faceSize = kEnvCubeSize >> mip;

            // LINEAR roughness -> mip. The eventual lookup must use exactly
            // this mapping; a mismatch is invisible to the eye.
            const float roughness =
                static_cast<float>(mip) / static_cast<float>(kEnvCubeMips - 1);

            D3D12_VIEWPORT viewport = { 0.0f, 0.0f,
                                        static_cast<float>(faceSize),
                                        static_cast<float>(faceSize), 0.0f, 1.0f };
            D3D12_RECT scissor = { 0, 0,
                                   static_cast<LONG>(faceSize),
                                   static_cast<LONG>(faceSize) };
            cmd->RSSetViewports(1, &viewport);
            cmd->RSSetScissorRects(1, &scissor);

            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvStart;
            rtv.ptr += static_cast<SIZE_T>(face * kEnvCubeMips + mip) * m_rtvDescSize;
            cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

            struct
            {
                uint32_t faceIndex;
                float    roughness;
                uint32_t sampleCount;
                float    invFaceSize;
            } constants = { face, roughness, kEnvPrefilterSamples,
                            1.0f / static_cast<float>(faceSize) };
            cmd->SetGraphicsRoot32BitConstants(0, 4, &constants, 0);

            cmd->DrawInstanced(3, 1, 0, 0);
        }
    }

    // Copy every subresource out for assertions 1.4 and 1.5, then park the cube
    // in the state both the raster and the DXR consumers will need.
    D3D12_RESOURCE_BARRIER toCopy =
        Transition(m_cube.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &toCopy);

    for (uint32_t sub = 0; sub < kEnvCubeMips * 6; ++sub)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource       = m_cubeReadback.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = m_cubeFootprints[sub];

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource        = m_cube.Get();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = sub;

        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER toRead =
        Transition(m_cube.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kCubeShaderReadState);
    cmd->ResourceBarrier(1, &toRead);
}

void EnvironmentIBL::RecordVerification(D3D12Device& device)
{
    ID3D12GraphicsCommandList* cmd = device.CmdList();

    // --- render the direction cubemap through the SAME face table ------------
    D3D12_RESOURCE_BARRIER dirToTarget =
        Transition(m_directionCube.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &dirToTarget);

    cmd->SetPipelineState(m_psoDirection.Get());

    D3D12_VIEWPORT dirViewport = { 0.0f, 0.0f,
                                   static_cast<float>(kEnvDirectionCubeSize),
                                   static_cast<float>(kEnvDirectionCubeSize), 0.0f, 1.0f };
    D3D12_RECT dirScissor = { 0, 0,
                              static_cast<LONG>(kEnvDirectionCubeSize),
                              static_cast<LONG>(kEnvDirectionCubeSize) };
    cmd->RSSetViewports(1, &dirViewport);
    cmd->RSSetScissorRects(1, &dirScissor);

    D3D12_CPU_DESCRIPTOR_HANDLE dirRtvStart =
        m_directionRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t face = 0; face < 6; ++face)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = dirRtvStart;
        rtv.ptr += static_cast<SIZE_T>(face) * m_rtvDescSize;
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        struct
        {
            uint32_t faceIndex;
            float    roughness;
            uint32_t sampleCount;
            float    invFaceSize;
        } constants = { face, 0.0f, 1u, 1.0f / static_cast<float>(kEnvDirectionCubeSize) };
        cmd->SetGraphicsRoot32BitConstants(0, 4, &constants, 0);

        cmd->DrawInstanced(3, 1, 0, 0);
    }

    D3D12_RESOURCE_BARRIER dirToRead =
        Transition(m_directionCube.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &dirToRead);

    // --- the two 64x1 probe passes -------------------------------------------
    ID3D12DescriptorHeap* heaps[] = { m_probeSrvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootConstantBufferView(1, m_probeDirectionCB->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(2, m_probeSrvHeap->GetGPUDescriptorHandleForHeapStart());

    D3D12_VIEWPORT probeViewport = { 0.0f, 0.0f, static_cast<float>(kEnvProbeCount),
                                     1.0f, 0.0f, 1.0f };
    D3D12_RECT probeScissor = { 0, 0, static_cast<LONG>(kEnvProbeCount), 1 };
    cmd->RSSetViewports(1, &probeViewport);
    cmd->RSSetScissorRects(1, &probeScissor);

    D3D12_CPU_DESCRIPTOR_HANDLE probeRtv =
        m_probeRtvHeap->GetCPUDescriptorHandleForHeapStart();

    const float poison[4] = { -1.0f, -1.0f, -1.0f, -1.0f };

    ID3D12PipelineState* probePsos[2] = { m_psoSkyProbe.Get(), m_psoDirectionProbe.Get() };
    for (uint32_t pass = 0; pass < 2; ++pass)
    {
        if (pass > 0)
        {
            D3D12_RESOURCE_BARRIER back =
                Transition(m_probeTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->ResourceBarrier(1, &back);
        }

        cmd->OMSetRenderTargets(1, &probeRtv, FALSE, nullptr);
        cmd->ClearRenderTargetView(probeRtv, poison, 0, nullptr);
        cmd->SetPipelineState(probePsos[pass]);
        cmd->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER toCopy =
            Transition(m_probeTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1, &toCopy);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        footprint.Offset             = static_cast<UINT64>(pass) * kEnvProbeCount * 16ull;
        footprint.Footprint.Format   = DXGI_FORMAT_R32G32B32A32_FLOAT;
        footprint.Footprint.Width    = kEnvProbeCount;
        footprint.Footprint.Height   = 1;
        footprint.Footprint.Depth    = 1;
        footprint.Footprint.RowPitch = kEnvProbeCount * 16u;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource       = m_probeReadback.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource        = m_probeTarget.Get();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    // Leave the probe target where the next bake expects to find it.
    D3D12_RESOURCE_BARRIER restore =
        Transition(m_probeTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &restore);
}

bool EnvironmentIBL::ReadbackAndValidate()
{
    m_validation = EnvironmentIBLValidation{};
    m_validation.built = true;

    // Which of the six (dominant axis, sign) buckets the query set reaches.
    // Computed from the direction array itself, so it measures the set that was
    // actually uploaded rather than the one the generator intended.
    {
        bool faceSeen[6] = { false, false, false, false, false, false };
        for (uint32_t i = 0; i < kEnvProbeCount; ++i)
        {
            const core::Vec3f& d = m_probeDirections[i];
            const float ax = std::fabs(d.x);
            const float ay = std::fabs(d.y);
            const float az = std::fabs(d.z);

            uint32_t bucket;
            if (ax >= ay && ax >= az)      bucket = (d.x >= 0.0f) ? 0u : 1u;
            else if (ay >= az)             bucket = (d.y >= 0.0f) ? 2u : 3u;
            else                           bucket = (d.z >= 0.0f) ? 4u : 5u;
            faceSeen[bucket] = true;
        }
        uint32_t covered = 0;
        for (bool seen : faceSeen)
            covered += seen ? 1u : 0u;
        m_validation.probeFacesCovered = covered;
    }

    // ---- probe readback: assertions 1.2 and 1.3 -----------------------------
    {
        const float* probe = nullptr;
        D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(2ull * kEnvProbeCount * 16ull) };
        HRESULT hr = m_probeReadback->Map(0, &readRange,
                                          reinterpret_cast<void**>(const_cast<float**>(&probe)));
        if (FAILED(hr) || !probe)
        {
            core::Log::Errorf("Failed to map IBL probe readback: 0x%08X", hr);
            return false;
        }

        const float* skyRow = probe;
        const float* dirRow = probe + kEnvProbeCount * 4;

        float worstSky = 0.0f;
        uint32_t skySlots = 0;
        for (uint32_t i = 0; i < kEnvProbeCount; ++i)
        {
            // w == 1 is the shader's "I ran here" witness against the -1 poison
            // clear. Counting written slots BEFORE comparing values is the whole
            // point: a comparison both sides satisfy vacuously is not a
            // comparison.
            if (skyRow[i * 4 + 3] != 1.0f)
                continue;
            ++skySlots;

            const core::Vec3f expected = core::SkyRadiance(m_probeDirections[i]);
            worstSky = (std::max)(worstSky, std::fabs(skyRow[i * 4 + 0] - expected.x));
            worstSky = (std::max)(worstSky, std::fabs(skyRow[i * 4 + 1] - expected.y));
            worstSky = (std::max)(worstSky, std::fabs(skyRow[i * 4 + 2] - expected.z));
        }
        m_validation.skySlots      = skySlots;
        m_validation.worstSkyDelta = worstSky;

        float worstDot = 1.0f;
        uint32_t dirSlots = 0;
        for (uint32_t i = 0; i < kEnvProbeCount; ++i)
        {
            if (dirRow[i * 4 + 3] != 1.0f)
                continue;
            ++dirSlots;

            const core::Vec3f sampled =
                core::Vec3f{ dirRow[i * 4 + 0], dirRow[i * 4 + 1], dirRow[i * 4 + 2] }.Normalized();
            const core::Vec3f query = m_probeDirections[i].Normalized();
            worstDot = (std::min)(worstDot, sampled.Dot(query));
        }
        m_validation.directionSlots    = dirSlots;
        m_validation.worstDirectionDot = (dirSlots == 0) ? 0.0f : worstDot;

        const D3D12_RANGE noWrite = { 0, 0 };
        m_probeReadback->Unmap(0, &noWrite);
    }

    // ---- cube readback: assertions 1.4 and 1.5 ------------------------------
    {
        const uint8_t* base = nullptr;
        D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(m_cubeReadbackBytes) };
        HRESULT hr = m_cubeReadback->Map(0, &readRange,
                                         reinterpret_cast<void**>(const_cast<uint8_t**>(&base)));
        if (FAILED(hr) || !base)
        {
            core::Log::Errorf("Failed to map IBL cube readback: 0x%08X", hr);
            return false;
        }

        for (uint32_t mip = 0; mip < kEnvCubeMips; ++mip)
        {
            // Double accumulators. The variance here is ~2.6e-3 against a mean
            // near 0.3, so sumSq/n - mean^2 in float would lose most of the
            // signal to cancellation.
            double sum   = 0.0;
            double sumSq = 0.0;
            uint64_t texels = 0;

            for (uint32_t face = 0; face < 6; ++face)
            {
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp =
                    m_cubeFootprints[mip + face * kEnvCubeMips];
                const uint32_t width  = fp.Footprint.Width;
                const uint32_t height = fp.Footprint.Height;

                for (uint32_t y = 0; y < height; ++y)
                {
                    const uint16_t* row = reinterpret_cast<const uint16_t*>(
                        base + fp.Offset + static_cast<uint64_t>(y) * fp.Footprint.RowPitch);
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const float r = HalfToFloat(row[x * 4 + 0]);
                        const float g = HalfToFloat(row[x * 4 + 1]);
                        const float b = HalfToFloat(row[x * 4 + 2]);
                        const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                        sum   += lum;
                        sumSq += lum * lum;
                        ++texels;
                    }
                }
            }

            const double mean = (texels == 0) ? 0.0 : sum / static_cast<double>(texels);
            const double var  = (texels == 0)
                                    ? 0.0
                                    : (std::max)(0.0, sumSq / static_cast<double>(texels) - mean * mean);
            m_validation.mipMeanLuminance[mip] = static_cast<float>(mean);
            m_validation.mipVariance[mip]      = static_cast<float>(var);
        }

        const D3D12_RANGE noWrite = { 0, 0 };
        m_cubeReadback->Unmap(0, &noWrite);
    }

    const float mean0 = m_validation.mipMeanLuminance[0];
    float worstDrift = 0.0f;
    for (uint32_t mip = 0; mip < kEnvCubeMips; ++mip)
    {
        const float drift = (mean0 > 1e-6f)
                                ? std::fabs(m_validation.mipMeanLuminance[mip] - mean0) / mean0
                                : 1.0f;
        worstDrift = (std::max)(worstDrift, drift);
    }
    m_validation.worstMipEnergyDrift = worstDrift;

    // Variance must fall as roughness rises. The VACUITY FLOOR comes first and
    // is the load-bearing half: an all-zero cube trivially satisfies "each mip
    // is not greater than the last", so without a floor this assertion passes on
    // a cube that was never rendered at all.
    bool decreasing = m_validation.mipVariance[0] > 1e-6f;
    for (uint32_t mip = kEnvVarianceFirstMip; mip < kEnvVarianceLastMip; ++mip)
    {
        if (!(m_validation.mipVariance[mip + 1] < m_validation.mipVariance[mip]))
            decreasing = false;
    }
    // The full-range claim, where the signal is unambiguous. See
    // kEnvVarianceFirstMip in the header for why this carries the "the chain
    // really is progressively blurring" claim instead of the mip0->mip1 step.
    if (!(m_validation.mipVariance[kEnvVarianceLastMip] <
          m_validation.mipVariance[kEnvVarianceFirstMip] * kEnvVarianceFalloff))
        decreasing = false;

    m_validation.varianceDecreasing = decreasing;

    return true;
}

// =============================================================================
// Markers
// =============================================================================
// Structured [SMOKE] lines rather than prose, so rewording a human-readable log
// message cannot silently disarm the harness. Integers are %u and verdicts are
// literal "pass"/"fail"/"yes"/"no": Assert-Marker is a PowerShell STRING
// compare, so "1.0" would not match "1".
bool EnvironmentIBL::LogMarkers(uint32_t descriptorSlot, uint32_t firstMaterialSlot) const
{
    if (!m_validation.built)
    {
        core::Log::Error("[SMOKE] ibl_env=fail");
        return false;
    }

    const bool reservationOk = firstMaterialSlot > descriptorSlot;

    const bool directionOk = (m_validation.directionSlots == kEnvProbeCount) &&
                             (m_validation.probeFacesCovered == 6u) &&
                             (m_validation.worstDirectionDot > kEnvDirectionDotTolerance);
    const bool skyOk       = (m_validation.skySlots == kEnvProbeCount) &&
                             (m_validation.worstSkyDelta < kEnvSkyAgreementTolerance);
    const bool energyOk    = m_validation.worstMipEnergyDrift < kEnvMipEnergyTolerance;
    const bool varianceOk  = m_validation.varianceDecreasing;

    core::Log::Infof("[SMOKE] ibl_env=%s ibl_env_slot=%u ibl_env_size=%u ibl_env_mips=%u "
                     "ibl_env_first_material_slot=%u",
                     reservationOk ? "ok" : "fail",
                     descriptorSlot, kEnvCubeSize, kEnvCubeMips, firstMaterialSlot);

    core::Log::Infof("[SMOKE] ibl_direction_roundtrip=%s ibl_direction_slots=%u "
                     "ibl_probe_faces=%u ibl_direction_worst_dot=%.6f",
                     directionOk ? "pass" : "fail",
                     m_validation.directionSlots,
                     m_validation.probeFacesCovered,
                     m_validation.worstDirectionDot);

    core::Log::Infof("[SMOKE] ibl_sky_agreement=%s ibl_sky_slots=%u ibl_sky_worst_delta=%.8f",
                     skyOk ? "pass" : "fail",
                     m_validation.skySlots,
                     m_validation.worstSkyDelta);

    core::Log::Infof("[SMOKE] ibl_mip_energy=%s ibl_mip_energy_drift=%.6f",
                     energyOk ? "pass" : "fail", m_validation.worstMipEnergyDrift);

    core::Log::Infof("[SMOKE] ibl_mip_variance_decreasing=%s ibl_mip_variance_0=%.8f",
                     varianceOk ? "yes" : "no", m_validation.mipVariance[0]);

    for (uint32_t mip = 0; mip < kEnvCubeMips; ++mip)
        core::Log::Infof("  env mip %u: roughness %.3f  mean-luminance %.6f  variance %.8f",
                         mip,
                         static_cast<float>(mip) / static_cast<float>(kEnvCubeMips - 1),
                         m_validation.mipMeanLuminance[mip],
                         m_validation.mipVariance[mip]);

    if (!reservationOk)
        core::Log::Errorf("IBL descriptor reservation failed: the cube's SRV is at slot %u but "
                          "the material allocator starts handing out at %u, so a material "
                          "texture will overwrite it. Nothing samples the cube yet, so this "
                          "would otherwise surface in Stage 3 as 'reflections are wrong'.",
                          descriptorSlot, firstMaterialSlot);
    if (!directionOk)
        core::Log::Errorf("IBL direction round trip failed: %u/%u slots written, %u/6 cube "
                          "faces covered by the query set, worst dot %.6f (need > %.4f). "
                          "The cube face table in ibl_environment.hlsli is internally "
                          "inconsistent, or the probe direction set no longer reaches every "
                          "face.",
                          m_validation.directionSlots, kEnvProbeCount,
                          m_validation.probeFacesCovered,
                          m_validation.worstDirectionDot, kEnvDirectionDotTolerance);
    if (!skyOk)
        core::Log::Errorf("IBL sky agreement failed: %u/%u slots written, worst delta %.8f "
                          "(need < %.6f). shaders/sky_common.hlsli and core::SkyRadiance "
                          "have drifted apart.",
                          m_validation.skySlots, kEnvProbeCount,
                          m_validation.worstSkyDelta, kEnvSkyAgreementTolerance);
    if (!energyOk)
        core::Log::Errorf("IBL mip energy failed: worst drift %.6f (need < %.4f). The "
                          "prefilter is probably missing its weight-sum division.",
                          m_validation.worstMipEnergyDrift, kEnvMipEnergyTolerance);
    if (!varianceOk)
        core::Log::Errorf("IBL mip variance failed: variance[0]=%.8f (floor 1e-6), "
                          "variance[%u]=%.8f, variance[%u]=%.8f. Mips were not rendered, "
                          "or roughness does not rise with mip.",
                          m_validation.mipVariance[0],
                          kEnvVarianceFirstMip, m_validation.mipVariance[kEnvVarianceFirstMip],
                          kEnvVarianceLastMip,  m_validation.mipVariance[kEnvVarianceLastMip]);

    return reservationOk && directionOk && skyOk && energyOk && varianceOk;
}

void EnvironmentIBL::WriteCubeSRV(ID3D12Device* device,
                                  D3D12_CPU_DESCRIPTOR_HANDLE destination) const
{
    if (!device || !m_cube)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                          = kEnvCubeFormat;
    srv.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MostDetailedMip     = 0;
    srv.TextureCube.MipLevels           = kEnvCubeMips;
    srv.TextureCube.ResourceMinLODClamp = 0.0f;

    device->CreateShaderResourceView(m_cube.Get(), &srv, destination);
}

void EnvironmentIBL::Shutdown()
{
    m_psoDirectionProbe.Reset();
    m_psoSkyProbe.Reset();
    m_psoDirection.Reset();
    m_psoPrefilter.Reset();
    m_rootSignature.Reset();
    m_probeDirectionCB.Reset();
    m_probeReadback.Reset();
    m_probeRtvHeap.Reset();
    m_probeTarget.Reset();
    m_probeSrvHeap.Reset();
    m_directionRtvHeap.Reset();
    m_directionCube.Reset();
    m_cubeReadback.Reset();
    m_cubeRtvHeap.Reset();
    m_cube.Reset();
    m_bakedRevision = kUnbakedRevision;
}

} // namespace render
