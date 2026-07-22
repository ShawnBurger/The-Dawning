// =============================================================================
// render/terrain_height_probe.cpp
// =============================================================================
#include "terrain_height_probe.h"

#include "d3d12_device.h"
#include "shader_utils.h"
#include "core/log.h"
#include "core/planet_height.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace render
{
namespace
{

// Verdict thresholds, CALIBRATED FROM MEASUREMENT (not chosen by reflex).
//
// Two channels, two tolerances, because the two twins do not — cannot — agree to
// the sky probe's 3e-8. That figure holds because the sky is a trivial affine
// blend; PlanetHeight is 5-octave domain-warped fBm over the Hoskins "hash without
// sine". Its CPU/GPU divergence is NOT simple mul-add FMA fusion — forcing the GPU
// arithmetic `precise` (no contraction) moved the worst delta by <1e-4. It is the
// GPU dot-product instruction (dp3, used by mul(kNoiseRot,·) and Hash13's dot)
// rounding its 3-term reduction differently from the CPU's sequential adds, plus
// frac() precision once the warped, seed-offset coordinate grows large — and it
// SCALES WITH COORDINATE MAGNITUDE: generic (seed 7) agrees to <1e-3, Moon (seed
// 33) to ~3e-2. MEASURED worst raw delta 0.0294; that is the reproducibility floor
// of this fBm, confirmed rather than assumed. A bit-exact INTEGER hash (bit ops are
// exact on both CPU and GPU) would collapse it to ~1e-6, which also matters for P5
// landing collision (CPU height must equal the visible surface); that is a
// deliberate terrain follow-on — it changes the shipped noise, so it is NOT folded
// in here.
//
// So this probe is a GROSS-DRIFT guard, not a bit-exactness guard, and it is
// honest about which: a one-sided edit to any enumerated constant (a hash factor,
// an octave count, kNoiseRot, lacunarity 2.02, gain 0.5, seed derivation) reshuffles
// the field and moves the delta to O(0.1..0.5) — several times these bounds, so it
// trips (watched-failing). It cannot catch a sub-3% drift; nothing short of the
// integer-hash rewrite can, and the comment says so.
//
//   kRawTolerance   guards the RAW continent field (the whole enumerated drift
//                   surface). measured floor 0.0294.
//   kElevTolerance  guards the final elevation branch (coast mask, ridged
//                   mountains, craters). Looser: the type-0 coast smoothstep
//                   amplifies the raw floor ~1/coastWidth at the shoreline (where
//                   h == seaLevel). measured floor 0.0616.
constexpr float kRawTolerance  = 5.0e-2f;   // raw field:      measured floor 0.0294
constexpr float kElevTolerance = 1.5e-1f;   // final elevation: measured floor 0.0616

// The height field must actually VARY across the query set, or a twin that
// returned a constant would agree with a constant vacuously. PlanetHeight spans
// roughly [0,1]; require at least this much spread across the 64 slots.
constexpr float kSpreadFloor = 0.05f;

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

// Dominant-axis bucket of a unit direction: 0..5 = (+X,-X,+Y,-Y,+Z,-Z). Used only
// by the coverage vacuity guard, so a direction set degenerated onto one face
// cannot pass while testing one sixth of the field.
uint32_t DominantAxisBucket(const core::Vec3f& d)
{
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    if (ax >= ay && ax >= az) return d.x >= 0.0f ? 0u : 1u;
    if (ay >= az)             return d.y >= 0.0f ? 2u : 3u;
    return d.z >= 0.0f ? 4u : 5u;
}

} // namespace

bool TerrainHeightProbe::Init(ID3D12Device* device)
{
    BuildQuerySet();
    if (!CreateRootSignature(device)) return false;
    if (!CreatePipeline(device))      return false;
    if (!CreateResources(device))     return false;
    m_ready = true;
    return true;
}

// One generator: 16 directions x 4 body types. The 16 directions are the 6 cube
// face axes (covering all 6 dominant-axis buckets), the 8 cube corners, and 2
// off-axis directions, all unit length. The 4 types mirror ConfigForTerrainBody
// in app.cpp: 0 Earth (coastline mask), 1 Mars, 2 Moon, 3 generic.
void TerrainHeightProbe::BuildQuerySet()
{
    const float k = 0.57735026919f; // 1/sqrt(3)
    const core::Vec3f dirs[kDirs] = {
        { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1},
        { k, k, k}, { k, k,-k}, { k,-k, k}, { k,-k,-k},
        {-k, k, k}, {-k, k,-k}, {-k,-k, k}, {-k,-k,-k},
        { 0.267261f, 0.534522f, 0.801784f },   // (1,2,3) normalised
        {-0.371391f, 0.557086f,-0.742781f },   // (-2,3,-4) normalised
    };

    // { type, seed, seaLevel, coastWidth } per body — the shipped configs.
    struct Cfg { int type; float seed, seaLevel, coastWidth; };
    const Cfg cfgs[kTypes] = {
        { 0, 11.0f, 0.52f, 0.02f },   // Earth
        { 1, 22.0f, 1.00f, 0.00f },   // Mars
        { 2, 33.0f, 1.00f, 0.00f },   // Moon
        { 3,  7.0f, 1.00f, 0.00f },   // generic
    };

    for (uint32_t t = 0; t < kTypes; ++t)
        for (uint32_t d = 0; d < kDirs; ++d)
        {
            const uint32_t slot = t * kDirs + d;
            m_dir[slot]        = dirs[d];
            m_type[slot]       = cfgs[t].type;
            m_seed[slot]       = cfgs[t].seed;
            m_seaLevel[slot]   = cfgs[t].seaLevel;
            m_coastWidth[slot] = cfgs[t].coastWidth;
        }
}

bool TerrainHeightProbe::CreateRootSignature(ID3D12Device* device)
{
    // One root CBV at b0 — the query set. No SRV/sampler: the probe reads no
    // textures, it only evaluates PlanetHeight from planet_noise.hlsli.
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor       = { 0, 0 }; // b0, space0
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 1;
    rs.pParameters   = &param;
    rs.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob)
            core::Log::Errorf("Terrain height probe root sig: %s",
                              static_cast<const char*>(errBlob->GetBufferPointer()));
        return false;
    }
    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                     sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_rootSig));
    if (FAILED(hr))
    {
        core::Log::Errorf("Terrain height probe CreateRootSignature failed: 0x%08X", hr);
        return false;
    }
    m_rootSig->SetName(L"TerrainHeightProbeRootSig");
    return true;
}

bool TerrainHeightProbe::CreatePipeline(ID3D12Device* device)
{
    // Shared fullscreen-triangle VS; its output { SV_POSITION, TEXCOORD0 } matches
    // the probe PS. Compiling the PS under FXC ps_5_1 /WX at startup is itself
    // evidence that planet_noise.hlsli stays inside SM 5.1 as the raster path needs.
    auto vs = CompileShaderFromFile(L"shaders/bloom_vs.hlsl", "main", "vs_5_1");
    auto ps = CompileShaderFromFile(L"shaders/planet_height_probe_ps.hlsl",
                                    "main", "ps_5_1");
    if (!vs || !ps)
    {
        core::Log::Error("Failed to compile terrain height probe shaders");
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_rootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0]    = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pso.SampleDesc       = { 1, 0 };
    pso.SampleMask       = UINT_MAX;

    HRESULT hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso));
    if (FAILED(hr))
    {
        core::Log::Errorf("Terrain height probe PSO creation failed: 0x%08X", hr);
        return false;
    }
    m_pso->SetName(L"TerrainHeightProbePSO");
    return true;
}

bool TerrainHeightProbe::CreateResources(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // --- kSlots x 1 probe target, poison-cleared to w = -1 -------------------
    D3D12_RESOURCE_DESC targetDesc = {};
    targetDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    targetDesc.Width            = kSlots;
    targetDesc.Height           = 1;
    targetDesc.DepthOrArraySize = 1;
    targetDesc.MipLevels        = 1;
    targetDesc.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
    targetDesc.SampleDesc       = { 1, 0 };
    targetDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE poison = {};
    poison.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    poison.Color[0] = poison.Color[1] = poison.Color[2] = poison.Color[3] = -1.0f;

    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &targetDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &poison, IID_PPV_ARGS(&m_target));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create terrain height probe target: 0x%08X", hr);
        return false;
    }
    m_target->SetName(L"TerrainHeightProbeTarget");

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create terrain height probe RTV heap: 0x%08X", hr);
        return false;
    }
    m_rtvHeap->SetName(L"TerrainHeightProbeRTVHeap");
    device->CreateRenderTargetView(m_target.Get(), nullptr,
                                   m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    // --- readback buffer: one kSlots-texel row. kSlots*16 = 1024, already a legal
    //     row-pitch / placed-footprint multiple, no padding arithmetic. ----------
    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width            = static_cast<UINT64>(kSlots) * 16ull;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc       = { 1, 0 };
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = device->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create terrain height probe readback: 0x%08X", hr);
        return false;
    }
    m_readback->SetName(L"TerrainHeightProbeReadback");

    // --- upload CB: 64 dir float4 + 64 param float4 = 2048 B (256-aligned) -----
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc = bufDesc;
    cbDesc.Width = static_cast<UINT64>(kSlots) * 16ull * 2ull; // dir + param arrays

    hr = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_cb));
    if (FAILED(hr))
    {
        core::Log::Errorf("Failed to create terrain height probe CB: 0x%08X", hr);
        return false;
    }
    m_cb->SetName(L"TerrainHeightProbeCB");

    UploadConstants();
    return true;
}

// Writes the query set into the CB verbatim: g_dir[64] then g_param[64], matching
// the cbuffer layout in planet_height_probe_ps.hlsl. The CPU comparison in
// ReadbackAndValidate reads the SAME m_dir/m_type/... arrays, so there is no
// second source of inputs to drift.
void TerrainHeightProbe::UploadConstants()
{
    float* mapped = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    if (FAILED(m_cb->Map(0, &noRead, reinterpret_cast<void**>(&mapped))))
    {
        core::Log::Error("Terrain height probe CB map failed");
        return;
    }
    // g_dir[64]: xyz = direction, w = 0.
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        mapped[i * 4 + 0] = m_dir[i].x;
        mapped[i * 4 + 1] = m_dir[i].y;
        mapped[i * 4 + 2] = m_dir[i].z;
        mapped[i * 4 + 3] = 0.0f;
    }
    // g_param[64]: x = type, y = seed, z = seaLevel, w = coastWidth.
    float* param = mapped + kSlots * 4;
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        param[i * 4 + 0] = static_cast<float>(m_type[i]);
        param[i * 4 + 1] = m_seed[i];
        param[i * 4 + 2] = m_seaLevel[i];
        param[i * 4 + 3] = m_coastWidth[i];
    }
    m_cb->Unmap(0, nullptr);
}

void TerrainHeightProbe::Record(D3D12Device& device)
{
    ID3D12GraphicsCommandList* cmd = device.CmdList();

    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_cb->GetGPUVirtualAddress());
    cmd->SetPipelineState(m_pso.Get());

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(kSlots), 1.0f, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, static_cast<LONG>(kSlots), 1 };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float poison[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtv, poison, 0, nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER toCopy =
        Transition(m_target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &toCopy);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    footprint.Offset             = 0;
    footprint.Footprint.Format   = DXGI_FORMAT_R32G32B32A32_FLOAT;
    footprint.Footprint.Width    = kSlots;
    footprint.Footprint.Height   = 1;
    footprint.Footprint.Depth    = 1;
    footprint.Footprint.RowPitch = kSlots * 16u;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource       = m_readback.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource        = m_target.Get();
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Leave the target in RENDER_TARGET, its creation/rest state.
    D3D12_RESOURCE_BARRIER restore =
        Transition(m_target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &restore);
}

bool TerrainHeightProbe::RunAndValidate(D3D12Device& device)
{
    if (!m_ready)
    {
        // The probe failed to build (e.g. a driver that could not compile the
        // shader). Do not brick the engine; the smoke harness still asserts the
        // marker's PRESENCE, so this absence is caught there.
        core::Log::Warn("Terrain height probe not initialised; skipping validation");
        return true;
    }
    if (m_ran) return true;
    m_ran = true;

    if (!device.WaitForCurrentFrame() || !device.ResetCommandList())
    {
        core::Log::Error("Unable to begin terrain height probe command list");
        return false;
    }

    Record(device);

    const HRESULT closeHr = device.CmdList()->Close();
    if (FAILED(closeHr))
    {
        core::Log::Errorf("Terrain height probe command list Close failed: 0x%08X", closeHr);
        return false;
    }
    ID3D12CommandList* lists[] = { device.CmdList() };
    device.CmdQueue()->ExecuteCommandLists(1, lists);
    if (!device.WaitForGpu())
        return false;

    return ReadbackAndValidate();
}

bool TerrainHeightProbe::ReadbackAndValidate()
{
    float* row = nullptr;
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(kSlots) * 16 };
    if (FAILED(m_readback->Map(0, &readRange, reinterpret_cast<void**>(&row))))
    {
        core::Log::Error("Terrain height probe readback map failed");
        return false;
    }

    uint32_t slots = 0;
    uint32_t bucketMask = 0;
    float    worstRaw  = 0.0f;   // raw continent field CPU/GPU delta
    float    worstElev = 0.0f;   // final elevation CPU/GPU delta
    float    minH =  1.0e30f, maxH = -1.0e30f;
    int      worstRawSlot = -1, worstElevSlot = -1;

    for (uint32_t i = 0; i < kSlots; ++i)
    {
        // w == +1 witness against the -1 poison: count only slots that ran.
        if (row[i * 4 + 3] != 1.0f) continue;
        ++slots;
        bucketMask |= (1u << DominantAxisBucket(m_dir[i]));

        const float gpuElev = row[i * 4 + 0];   // r = PlanetHeight (final)
        const float gpuRaw  = row[i * 4 + 1];   // g = PlanetHeightRaw

        const core::Vec3f seedO = core::PlanetSeedOffset(m_seed[i]);
        const float cpuRaw  = core::PlanetHeightRaw(m_dir[i], seedO);
        const float cpuElev = core::PlanetHeight(m_dir[i], m_type[i], m_seed[i],
                                                 m_seaLevel[i], m_coastWidth[i]);

        const float dRaw  = std::fabs(gpuRaw  - cpuRaw);
        const float dElev = std::fabs(gpuElev - cpuElev);
        if (dRaw  > worstRaw)  { worstRaw  = dRaw;  worstRawSlot  = static_cast<int>(i); }
        if (dElev > worstElev) { worstElev = dElev; worstElevSlot = static_cast<int>(i); }
        minH = (std::min)(minH, gpuElev);
        maxH = (std::max)(maxH, gpuElev);
    }

    const D3D12_RANGE noWrite = { 0, 0 };
    m_readback->Unmap(0, &noWrite);

    uint32_t buckets = 0;
    for (uint32_t b = 0; b < 6; ++b) buckets += (bucketMask >> b) & 1u;
    const float spread = (slots > 0) ? (maxH - minH) : 0.0f;

    const bool ok = (slots == kSlots) &&
                    (buckets == 6) &&
                    (spread > kSpreadFloor) &&
                    (worstRaw  < kRawTolerance) &&
                    (worstElev < kElevTolerance);

    // Verdicts are literal strings and counts are %u — the smoke harness compares
    // them as text. worst_raw is the load-bearing number (the shared-noise guard);
    // worst_elev rides the coast-amplified elevation and is reported for context.
    core::Log::Infof(
        "[SMOKE] terrain_height_agreement=%s terrain_height_slots=%u "
        "terrain_height_buckets=%u terrain_height_worst_raw=%.8f "
        "terrain_height_worst_elev=%.8f terrain_height_spread=%.6f",
        ok ? "pass" : "fail", slots, buckets, worstRaw, worstElev, spread);

    if (!ok)
    {
        core::Log::Errorf(
            "[ERR ] Terrain height twin disagreement: slots=%u/%u buckets=%u/6 "
            "spread=%.6f (floor %.6f) worst_raw=%.8f (tol %.8f, slot %d type %d) "
            "worst_elev=%.8f (tol %.8f, slot %d type %d)",
            slots, kSlots, buckets, spread, kSpreadFloor,
            worstRaw,  kRawTolerance,  worstRawSlot,
            worstRawSlot  >= 0 ? m_type[worstRawSlot]  : -1,
            worstElev, kElevTolerance, worstElevSlot,
            worstElevSlot >= 0 ? m_type[worstElevSlot] : -1);
    }
    return ok;
}

} // namespace render
