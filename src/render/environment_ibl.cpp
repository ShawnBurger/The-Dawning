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
    // Two SRVs in one table: t0 the direction cubemap (assertion 1.2), t1 the
    // prefiltered environment cube itself (Stage 3). APPEND puts them at
    // descriptors 0 and 1 of m_probeSrvHeap, in that order.
    D3D12_DESCRIPTOR_RANGE probeRanges[2] = {};
    probeRanges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    probeRanges[0].NumDescriptors                    = 1;
    probeRanges[0].BaseShaderRegister                = 0;
    probeRanges[0].RegisterSpace                     = 0;
    probeRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    probeRanges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    probeRanges[1].NumDescriptors                    = 1;
    probeRanges[1].BaseShaderRegister                = 1;
    probeRanges[1].RegisterSpace                     = 0;
    probeRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

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

    // t0: the direction cubemap, t1: the environment cube (verification only).
    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = _countof(probeRanges);
    params[2].DescriptorTable.pDescriptorRanges   = probeRanges;
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

    // Stages 2 and 3. These four include shaders/ibl_common.hlsli - the SAME
    // header basic_ps.hlsl includes - so compiling them here under ps_5_1 /WX is
    // the standing answer to IBL_DESIGN.md section 6.3's open question about
    // TextureCube and SamplerState as function parameters. It is checked every
    // launch, and the engine fails to start rather than failing quietly.
    auto shProbePs      = CompileShaderFromFile(L"shaders/ibl_eval_probe_ps.hlsl",
                                                "SHIrradianceProbePS", "ps_5_1");
    auto mirrorProbePs  = CompileShaderFromFile(L"shaders/ibl_eval_probe_ps.hlsl",
                                                "MirrorSpecularProbePS", "ps_5_1");
    auto envBRDFProbePs = CompileShaderFromFile(L"shaders/ibl_eval_probe_ps.hlsl",
                                                "EnvBRDFProbePS", "ps_5_1");
    auto mipSweepProbePs = CompileShaderFromFile(L"shaders/ibl_eval_probe_ps.hlsl",
                                                 "MipSweepProbePS", "ps_5_1");

    if (!vs || !prefilterPs || !directionPs || !skyProbePs || !dirProbePs ||
        !shProbePs || !mirrorProbePs || !envBRDFProbePs || !mipSweepProbePs)
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
        { shProbePs.Get(),       DXGI_FORMAT_R32G32B32A32_FLOAT, &m_psoSHProbe,       L"EnvironmentSHProbePSO" },
        { mirrorProbePs.Get(),   DXGI_FORMAT_R32G32B32A32_FLOAT, &m_psoMirrorProbe,   L"EnvironmentMirrorProbePSO" },
        { envBRDFProbePs.Get(),  DXGI_FORMAT_R32G32B32A32_FLOAT, &m_psoEnvBRDFProbe,  L"EnvironmentEnvBRDFProbePSO" },
        { mipSweepProbePs.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, &m_psoMipSweepProbe, L"EnvironmentMipSweepProbePSO" },
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

    // Shader-visible SRVs for the two cubes the probes read, in their own heap:
    // [0] the direction cube (assertion 1.2), [1] the environment cube itself
    // (Stages 2 and 3). This is a SECOND CBV_SRV_UAV heap and that is legal here
    // because it is bound only during the startup probe pass, before any frame
    // is recorded - the engine's one-bindable-heap rule is about a single point
    // in time, and no scene pass is open.
    D3D12_DESCRIPTOR_HEAP_DESC probeHeapDesc = {};
    probeHeapDesc.NumDescriptors = 2;
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
    D3D12_CPU_DESCRIPTOR_HANDLE probeSrvStart =
        m_probeSrvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(m_directionCube.Get(), &dirSrv, probeSrvStart);

    // [1] the prefiltered cube, with the FULL mip chain exposed - the mip sweep
    // probe is precisely a claim about which mip a roughness lands on, so a view
    // that clamped MipLevels to 1 would make that assertion vacuous.
    const uint32_t srvDescSize =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE envSrvHandle = probeSrvStart;
    envSrvHandle.ptr += srvDescSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC envSrv = {};
    envSrv.Format                          = kEnvCubeFormat;
    envSrv.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBE;
    envSrv.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    envSrv.TextureCube.MostDetailedMip     = 0;
    envSrv.TextureCube.MipLevels           = kEnvCubeMips;
    envSrv.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(m_cube.Get(), &envSrv, envSrvHandle);

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

    // One 64-texel row per probe pass; see enum EnvProbePass for the order.
    // 64 * 16 B is 1024, already a multiple of both the 256-byte row pitch
    // alignment and the 512-byte placed-footprint alignment, so each pass's row
    // starts on a legal offset with no padding arithmetic.
    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC probeReadbackDesc = {};
    probeReadbackDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    probeReadbackDesc.Width            = static_cast<UINT64>(kEnvPassCount) * kEnvProbeCount * 16ull;
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

    // 64 directions + 9 SH coefficients + 1 parameter row, rounded up to a
    // 256-byte boundary because a root CBV's address must be 256-aligned and the
    // resource base is the address here.
    D3D12_RESOURCE_DESC cbDesc = probeReadbackDesc;
    cbDesc.Width = 1280ull;   // (64 + 9 + 1) * 16 = 1184, aligned up

    hr = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_probeDirectionCB));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create IBL probe constant buffer: 0x%08X", hr);
        return false;
    }
    m_probeDirectionCB->SetName(L"EnvironmentProbeConstantBuffer");

    // Contents are written by UploadProbeConstants, which runs per bake because
    // the SH tail is not known until the projection has happened.
    return true;
}

// The (NdotV, roughness) grid the env-BRDF probe sweeps. Twin of DawningProbeGrid
// in shaders/ibl_eval_probe_ps.hlsl; the header says what watches that.
void EnvironmentIBL::EnvBRDFProbeTuple(uint32_t slot, float& NdotV, float& roughness)
{
    NdotV     = static_cast<float>(slot % 8u) / 7.0f;
    roughness = static_cast<float>(slot / 8u) / 7.0f;
}

// Writes the probe constant buffer: the direction set, then the SH coefficients
// and parameters the frame shaders will receive.
//
// THE SAME m_irradiance ARRAY GOES TO BOTH the GPU here and to
// Renderer::BeginFrame's CBPerFrame. That is the property that makes the
// agreement probe meaningful: it compares the CPU's own numbers against the
// HLSL's evaluation OF THOSE NUMBERS, so what is under test is the BASIS and
// nothing else. If the probe projected its own coefficients it would be testing
// two projections and telling you nothing about the one the frame uses.
bool EnvironmentIBL::UploadProbeConstants()
{
    float* mapped = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    HRESULT hr = m_probeDirectionCB->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to map IBL probe constant buffer: 0x%08X", hr);
        return false;
    }

    for (uint32_t i = 0; i < kEnvProbeCount; ++i)
    {
        mapped[i * 4 + 0] = m_probeDirections[i].x;
        mapped[i * 4 + 1] = m_probeDirections[i].y;
        mapped[i * 4 + 2] = m_probeDirections[i].z;
        mapped[i * 4 + 3] = 0.0f;
    }

    float* sh = mapped + kEnvProbeCount * 4;
    for (uint32_t k = 0; k < core::kSHCoefficientCount; ++k)
    {
        sh[k * 4 + 0] = m_irradiance.c[k].x;
        sh[k * 4 + 1] = m_irradiance.c[k].y;
        sh[k * 4 + 2] = m_irradiance.c[k].z;
        sh[k * 4 + 3] = 0.0f;
    }

    float* params = sh + core::kSHCoefficientCount * 4;
    params[0] = static_cast<float>(kEnvCubeMips);
    params[1] = 1.0f;
    params[2] = 1.0f;
    params[3] = 0.0f;

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

    // Diffuse irradiance, projected on the CPU, because it lives in a constant
    // buffer and there is no compute path in this project to read one back from
    // the GPU. The cost of that choice is a C++ mirror of the sky (see
    // src/core/sky_radiance.h) plus a C++ mirror of the SH basis (see
    // src/core/sh_irradiance.h); both are watched by probes below, which is what
    // makes IBL_DESIGN.md section 4's recommendation defensible rather than
    // merely cheap.
    //
    // Projected BEFORE the command list opens: it is pure CPU work and it must be
    // in the constant buffer before RecordVerification draws the pass that reads
    // it.
    m_irradiance = core::PackIrradianceCoefficients(
        core::ProjectSkyRadiance(core::kSHProjectionSamples));

    if (!UploadProbeConstants())
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

    // --- the 64x1 probe passes -----------------------------------------------
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

    // Order MUST match enum EnvProbePass: the readback row a pass writes is its
    // index, and ReadbackAndValidate indexes by the same enum.
    ID3D12PipelineState* probePsos[kEnvPassCount] = {
        m_psoSkyProbe.Get(),        // kEnvPassSkyAgreement
        m_psoDirectionProbe.Get(),  // kEnvPassDirectionRT
        m_psoSHProbe.Get(),         // kEnvPassSHIrradiance
        m_psoMirrorProbe.Get(),     // kEnvPassMirrorSpecular
        m_psoEnvBRDFProbe.Get(),    // kEnvPassEnvBRDF
        m_psoMipSweepProbe.Get(),   // kEnvPassMipSweep
    };
    for (uint32_t pass = 0; pass < kEnvPassCount; ++pass)
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

    // ---- probe readback: every pass -----------------------------------------
    {
        const float* probe = nullptr;
        D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(
            static_cast<uint64_t>(kEnvPassCount) * kEnvProbeCount * 16ull) };
        HRESULT hr = m_probeReadback->Map(0, &readRange,
                                          reinterpret_cast<void**>(const_cast<float**>(&probe)));
        if (FAILED(hr) || !probe)
        {
            core::Log::Errorf("Failed to map IBL probe readback: 0x%08X", hr);
            return false;
        }

        auto Row = [probe](uint32_t pass) { return probe + static_cast<size_t>(pass) * kEnvProbeCount * 4; };

        const float* skyRow = Row(kEnvPassSkyAgreement);
        const float* dirRow = Row(kEnvPassDirectionRT);

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

        // ---- Stage 2: the SH BASIS mirror ----------------------------------
        // The one assertion that can see a one-sided sign flip or permutation
        // between core::SHBasisL2 and DawningSHBasisL2. No CPU test can: the
        // HLSL is not linked into the test binary. Both sides consume the SAME
        // m_irradiance array, so the only variable under test is the basis.
        {
            const float* shRow = Row(kEnvPassSHIrradiance);
            float worst = 0.0f;
            float minLum = 1e30f;
            uint32_t slots = 0;
            for (uint32_t i = 0; i < kEnvProbeCount; ++i)
            {
                if (shRow[i * 4 + 3] != 1.0f)
                    continue;
                ++slots;

                const core::Vec3f expected =
                    core::EvaluateIrradiance(m_irradiance, m_probeDirections[i].Normalized());
                worst = (std::max)(worst, std::fabs(shRow[i * 4 + 0] - expected.x));
                worst = (std::max)(worst, std::fabs(shRow[i * 4 + 1] - expected.y));
                worst = (std::max)(worst, std::fabs(shRow[i * 4 + 2] - expected.z));

                const core::Vec3f got{ shRow[i * 4 + 0], shRow[i * 4 + 1], shRow[i * 4 + 2] };
                minLum = (std::min)(minLum, core::Luminance(got));
            }
            m_validation.shSlots        = slots;
            m_validation.worstSHDelta   = worst;
            m_validation.minSHLuminance = (slots == 0) ? 0.0f : minLum;
        }

        // ---- Stage 3.3: mirror specular ------------------------------------
        // rgb is DawningSpecularIBL at roughness 0 with N = V = d and F0 = 1;
        // w is the env-BRDF scalar the SHADER computed for that configuration.
        // Dividing w out leaves the prefiltered mip-0 fetch, which must be
        // core::SkyRadiance(d) - so this compares the SHIPPED sampling path
        // against the CPU sky WITHOUT re-implementing the Lazarov fit anywhere.
        {
            const float* row = Row(kEnvPassMirrorSpecular);
            float worstRel = 0.0f;
            float minLum   = 1e30f;
            uint32_t slots = 0;
            for (uint32_t i = 0; i < kEnvProbeCount; ++i)
            {
                const float scale = row[i * 4 + 3];
                // The w channel is doing double duty here: it is the env-BRDF
                // scalar AND the written-slot witness, since the poison clear
                // leaves -1 and a real scalar is strictly positive.
                if (!(scale > 0.0f))
                    continue;
                ++slots;

                const core::Vec3f expected = core::SkyRadiance(m_probeDirections[i].Normalized()) * scale;
                const core::Vec3f got{ row[i * 4 + 0], row[i * 4 + 1], row[i * 4 + 2] };

                const float denom = (std::max)(core::Luminance(expected), 1e-4f);
                worstRel = (std::max)(worstRel, std::fabs(got.x - expected.x) / denom);
                worstRel = (std::max)(worstRel, std::fabs(got.y - expected.y) / denom);
                worstRel = (std::max)(worstRel, std::fabs(got.z - expected.z) / denom);
                minLum   = (std::min)(minLum, core::Luminance(got));
            }
            m_validation.mirrorSlots        = slots;
            m_validation.worstMirrorRelError = worstRel;
            m_validation.minMirrorLuminance  = (slots == 0) ? 0.0f : minLum;
        }

        // ---- Stage 3.1: the env-BRDF fit's physical bounds ------------------
        // PHYSICAL PROPERTIES, NOT GOLDEN NUMBERS. IBL_DESIGN.md's own furnace
        // formulation for this assertion - "every result within 10% of the cube
        // mean" - is not satisfiable and contradicts the design's own section
        // 9.4: at roughness 1 and F0 = 1 the single-scattering split-sum returns
        // A + B = 0.452, a 55% energy loss that section 9.4 names as deliberately
        // uncorrected. Asserting 10% there would have failed for a reason the
        // design already accepted. What is asserted instead:
        //
        //   normal incidence, dielectric : F0*A + B must reduce to F0
        //   grazing, dielectric          : F0*A + B must approach unity
        //   anywhere, F0 = 1             : A + B in (floor, 1] - loses energy,
        //                                  never creates it
        //
        // The first two are what give an A/B SWAP teeth. At F0 = 1 the expression
        // is symmetric in A and B and a swap is completely invisible; only a
        // dielectric can see it, and the design's stated negative test omits that.
        {
            const float* row = Row(kEnvPassEnvBRDF);
            const float dielectricF0 = 0.04f;
            uint32_t slots = 0;
            float worstNormal   = 0.0f;
            // Sentinel, NOT 1.0. Initialising a running MINIMUM to the value the
            // assertion is trying to prove it exceeds makes the check pass when
            // no qualifying sample exists, and reports a fabricated number when
            // one does - both of which this initialiser did on its first run.
            float worstGrazing  = 1e30f;
            float worstExcess   = 0.0f;
            float worstShortfall = 1e30f;
            float worstDielectric = 0.0f;
            // The vacuity guard for the two smooth-row claims: they look at a
            // single grid point each, and a grid that no longer contains
            // (roughness 0, NdotV 0/1) would satisfy them by never evaluating.
            uint32_t smoothRowSamples = 0;

            for (uint32_t i = 0; i < kEnvProbeCount; ++i)
            {
                if (row[i * 4 + 3] != 1.0f)
                    continue;
                ++slots;

                const float A = row[i * 4 + 0];
                const float B = row[i * 4 + 1];

                float NdotV, roughness;
                EnvBRDFProbeTuple(i, NdotV, roughness);

                const float metalReflectance = A + B;
                worstExcess    = (std::max)(worstExcess, metalReflectance);
                worstShortfall = (std::min)(worstShortfall, metalReflectance);

                // The two Fresnel identities hold for a SMOOTH surface only; see
                // kEnvBRDFNormalIncidenceTolerance for the measurement that
                // established this restriction and why widening it back would be
                // asserting against physics rather than for it.
                const float dielectric = dielectricF0 * A + B;
                // The dielectric energy claim, over the WHOLE grid rather than
                // the smooth row. It is not covered by metalReflectance above:
                // at F0 = 1 the expression is symmetric in A and B, so the metal
                // reading is blind to exactly the asymmetries this one sees.
                worstDielectric = (std::max)(worstDielectric, dielectric);
                if (roughness <= 0.0f && NdotV >= 1.0f)
                {
                    worstNormal = (std::max)(worstNormal, std::fabs(dielectric - dielectricF0));
                    ++smoothRowSamples;
                }
                if (roughness <= 0.0f && NdotV <= 0.0f)
                {
                    worstGrazing = (std::min)(worstGrazing, dielectric);
                    ++smoothRowSamples;
                }
            }
            m_validation.envBRDFSlots              = slots;
            m_validation.envBRDFSmoothRowSamples   = smoothRowSamples;
            m_validation.worstNormalIncidenceError = worstNormal;
            m_validation.worstGrazingReflectance   = (worstGrazing > 1e29f) ? 0.0f : worstGrazing;
            m_validation.worstEnergyExcess         = worstExcess;
            m_validation.worstEnergyShortfall      = (worstShortfall > 1e29f) ? 0.0f : worstShortfall;
            m_validation.worstDielectricExcess     = worstDielectric;
        }

        // ---- Stage 3.2: roughness -> mip ------------------------------------
        // Prefiltered radiance along +Y, roughness swept 0..1. It must not fall,
        // and it must rise appreciably. An inverted or off-by-one mapping
        // reverses the sequence, which fails both halves.
        {
            const float* row = Row(kEnvPassMipSweep);
            uint32_t slots = 0;
            float worstStep = 0.0f;
            float first = 0.0f;
            float last  = 0.0f;
            bool  haveFirst = false;
            float previous = 0.0f;

            // Slot count is over the WHOLE row - "did every slot run" is a
            // question about the draw, not about the asserted sub-range - while
            // the monotonicity and rise claims stop at kEnvMipSweepLastSlot.
            for (uint32_t i = 0; i < kEnvProbeCount; ++i)
            {
                if (row[i * 4 + 3] != 1.0f)
                    continue;
                ++slots;

                if (i > kEnvMipSweepLastSlot)
                    continue;

                const core::Vec3f got{ row[i * 4 + 0], row[i * 4 + 1], row[i * 4 + 2] };
                const float lum = core::Luminance(got);

                if (!haveFirst) { first = lum; haveFirst = true; }
                else            { worstStep = (std::min)(worstStep, lum - previous); }
                previous = lum;
                last = lum;
            }
            m_validation.mipSweepSlots  = slots;
            m_validation.worstMipSweepStep = worstStep;
            m_validation.mipSweepRise      = last - first;
        }

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

    // ---- Stage 2 / Stage 3 verdicts -----------------------------------------
    // Every one of these leads with a SLOT COUNT, because a comparison neither
    // side reached is not a comparison, and every one carries a non-degeneracy
    // floor beside the agreement bound, because two implementations that both
    // return zero agree perfectly.
    m_validation.shAgreementOk =
        (m_validation.shSlots == kEnvProbeCount) &&
        (m_validation.worstSHDelta < kEnvSHAgreementTolerance) &&
        (m_validation.minSHLuminance > 0.1f);

    m_validation.mirrorOk =
        (m_validation.mirrorSlots == kEnvProbeCount) &&
        (m_validation.worstMirrorRelError < kEnvMirrorTolerance) &&
        (m_validation.minMirrorLuminance > 1e-4f);

    m_validation.envBRDFOk =
        (m_validation.envBRDFSlots == kEnvProbeCount) &&
        (m_validation.envBRDFSmoothRowSamples == 2u) &&
        (m_validation.worstNormalIncidenceError < kEnvBRDFNormalIncidenceTolerance) &&
        (m_validation.worstGrazingReflectance   > kEnvBRDFGrazingFloor) &&
        (m_validation.worstEnergyExcess         < kEnvBRDFEnergyCeiling) &&
        (m_validation.worstEnergyShortfall      > kEnvBRDFEnergyFloor) &&
        // The dielectric energy bound. Added because the F0 = 1 ceiling beside
        // it CANNOT see a dielectric overshoot - it reads A + B, and the
        // dielectric quantity is 0.04*A + B - so "no energy creation" was
        // asserted for one material and merely hoped for the other. It is a
        // wider bound than the metal one, honestly, because the fit really does
        // overshoot by 4.2% at grazing incidence on a smooth surface; see
        // kEnvBRDFDielectricEnergyCeiling for the measurement and the argument.
        (m_validation.worstDielectricExcess     < kEnvBRDFDielectricEnergyCeiling);

    m_validation.mipSweepOk =
        (m_validation.mipSweepSlots == kEnvProbeCount) &&
        (m_validation.worstMipSweepStep > -kEnvMipSweepStepTolerance) &&
        (m_validation.mipSweepRise      > kEnvMipSweepMinRise);

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

    core::Log::Infof("[SMOKE] ibl_sh_agreement=%s ibl_sh_slots=%u ibl_sh_worst_delta=%.8f "
                     "ibl_sh_min_luminance=%.6f",
                     m_validation.shAgreementOk ? "pass" : "fail",
                     m_validation.shSlots,
                     m_validation.worstSHDelta,
                     m_validation.minSHLuminance);

    core::Log::Infof("[SMOKE] ibl_spec_mirror=%s ibl_spec_mirror_slots=%u "
                     "ibl_spec_mirror_worst_rel=%.6f",
                     m_validation.mirrorOk ? "pass" : "fail",
                     m_validation.mirrorSlots,
                     m_validation.worstMirrorRelError);

    core::Log::Infof("[SMOKE] ibl_env_brdf=%s ibl_env_brdf_slots=%u "
                     "ibl_env_brdf_smooth_samples=%u "
                     "ibl_env_brdf_normal_err=%.6f ibl_env_brdf_grazing=%.6f "
                     "ibl_env_brdf_max_energy=%.6f ibl_env_brdf_min_energy=%.6f "
                     "ibl_env_brdf_max_dielectric=%.6f",
                     m_validation.envBRDFOk ? "pass" : "fail",
                     m_validation.envBRDFSlots,
                     m_validation.envBRDFSmoothRowSamples,
                     m_validation.worstNormalIncidenceError,
                     m_validation.worstGrazingReflectance,
                     m_validation.worstEnergyExcess,
                     m_validation.worstEnergyShortfall,
                     m_validation.worstDielectricExcess);

    core::Log::Infof("[SMOKE] ibl_spec_mip_monotonic=%s ibl_spec_mip_slots=%u "
                     "ibl_spec_mip_worst_step=%.8f ibl_spec_mip_rise=%.6f",
                     m_validation.mipSweepOk ? "pass" : "fail",
                     m_validation.mipSweepSlots,
                     m_validation.worstMipSweepStep,
                     m_validation.mipSweepRise);

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

    if (!m_validation.shAgreementOk)
        core::Log::Errorf("IBL SH agreement failed: %u/%u slots written, worst delta %.8f "
                          "(need < %.6f), min irradiance luminance %.6f (need > 0.1). "
                          "DawningSHBasisL2 in shaders/ibl_common.hlsli and core::SHBasisL2 "
                          "have drifted - a sign flip or permutation on ONE side does not "
                          "cancel and lights every surface from the wrong direction.",
                          m_validation.shSlots, kEnvProbeCount,
                          m_validation.worstSHDelta, kEnvSHAgreementTolerance,
                          m_validation.minSHLuminance);
    if (!m_validation.mirrorOk)
        core::Log::Errorf("IBL mirror specular failed: %u/%u slots written, worst relative "
                          "error %.6f (need < %.4f), min luminance %.6f. Mip 0 is not an exact "
                          "evaluation of the sky, or the reflection direction is wrong "
                          "(reflect(V, N) for reflect(-V, N) measures 1.03 here).",
                          m_validation.mirrorSlots, kEnvProbeCount,
                          m_validation.worstMirrorRelError, kEnvMirrorTolerance,
                          m_validation.minMirrorLuminance);
    if (!m_validation.envBRDFOk)
        core::Log::Errorf("IBL env-BRDF failed: %u/%u slots, normal-incidence error %.6f "
                          "(need < %.4f), grazing reflectance %.6f (need > %.2f), F0=1 "
                          "reflectance in [%.4f, %.4f] (need (%.2f, %.2f]), F0=0.04 peak "
                          "%.4f (need < %.2f). "
                          "DawningEnvBRDFApprox no longer behaves like a split-sum BRDF - "
                          "check for an A/B swap or a re-expression in terms of "
                          "DawningGeometrySmithG1, which uses a DIFFERENT Smith k.",
                          m_validation.envBRDFSlots, kEnvProbeCount,
                          m_validation.worstNormalIncidenceError, kEnvBRDFNormalIncidenceTolerance,
                          m_validation.worstGrazingReflectance, kEnvBRDFGrazingFloor,
                          m_validation.worstEnergyShortfall, m_validation.worstEnergyExcess,
                          kEnvBRDFEnergyFloor, kEnvBRDFEnergyCeiling,
                          m_validation.worstDielectricExcess, kEnvBRDFDielectricEnergyCeiling);
    if (!m_validation.mipSweepOk)
        core::Log::Errorf("IBL mip monotonicity failed: %u/%u slots, worst backward step %.8f "
                          "(need > %.5f), total rise %.6f (need > %.3f). roughness -> mip is "
                          "inverted or off by one, which is otherwise invisible - it reads as "
                          "'reflections are slightly too blurry'.",
                          m_validation.mipSweepSlots, kEnvProbeCount,
                          m_validation.worstMipSweepStep, -kEnvMipSweepStepTolerance,
                          m_validation.mipSweepRise, kEnvMipSweepMinRise);

    return reservationOk && directionOk && skyOk && energyOk && varianceOk &&
           m_validation.shAgreementOk && m_validation.mirrorOk &&
           m_validation.envBRDFOk && m_validation.mipSweepOk;
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
    m_psoMipSweepProbe.Reset();
    m_psoEnvBRDFProbe.Reset();
    m_psoMirrorProbe.Reset();
    m_psoSHProbe.Reset();
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
