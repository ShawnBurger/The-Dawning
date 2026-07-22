// =============================================================================
// render/debug_overlay.cpp - lightweight D3D12 debug overlay
// =============================================================================

#include "debug_overlay.h"
#include "shader_utils.h"
#include "../core/log.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>

namespace render
{

struct OverlayConstants
{
    float viewportSize[2];
    float overlaySize[2];
    float overlayPos[2];
    float opacity;
    float pad;
};

static uint32_t AlignTo(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

bool DebugOverlay::Init(D3D12Device& device)
{
    m_pixels.resize(kOverlayWidth * kOverlayHeight * 4);
    m_uploadPitch = AlignTo(kOverlayWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

    if (!CreatePipeline(device.Device())) return false;
    if (!CreateTextureResources(device.Device())) return false;
    if (!CreateTextResources()) return false;

    m_initialized = true;
    core::Log::Info("Debug overlay initialized (F3 toggles visibility)");
    return true;
}

void DebugOverlay::Shutdown()
{
    ReleaseTextResources();

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_uploadBuffers[i] && m_uploadMapped[i])
        {
            m_uploadBuffers[i]->Unmap(0, nullptr);
            m_uploadMapped[i] = nullptr;
        }
        m_uploadBuffers[i].Reset();
    }

    m_texture.Reset();
    m_srvHeap.Reset();
    m_pso.Reset();
    m_rootSig.Reset();
    m_initialized = false;
}

bool DebugOverlay::CreatePipeline(ID3D12Device* device)
{
    auto vsBytecode = CompileShaderFromFile(L"shaders/overlay_vs.hlsl", "main", "vs_5_1");
    auto psBytecode = CompileShaderFromFile(L"shaders/overlay_ps.hlsl", "main", "ps_5_1");
    if (!vsBytecode || !psBytecode)
    {
        core::Log::Error("Failed to compile debug overlay shaders");
        return false;
    }

    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = sizeof(OverlayConstants) / sizeof(uint32_t);
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof(rootParams);
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
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
            core::Log::Errorf("Overlay root signature error: %s",
                              static_cast<const char*>(errorBlob->GetBufferPointer()));
        return false;
    }

    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                     sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_rootSig));
    if (FAILED(hr))
    {
        core::Log::Errorf("Overlay CreateRootSignature failed: 0x%08X", hr);
        return false;
    }
    m_rootSig->SetName(L"DebugOverlayRootSignature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig.Get();
    psoDesc.VS.pShaderBytecode = vsBytecode->GetBufferPointer();
    psoDesc.VS.BytecodeLength = vsBytecode->GetBufferSize();
    psoDesc.PS.pShaderBytecode = psBytecode->GetBufferPointer();
    psoDesc.PS.BytecodeLength = psBytecode->GetBufferSize();
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc = { 1, 0 };
    psoDesc.SampleMask = UINT_MAX;

    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
    if (FAILED(hr))
    {
        core::Log::Errorf("Overlay CreateGraphicsPipelineState failed: 0x%08X", hr);
        return false;
    }
    m_pso->SetName(L"DebugOverlayPSO");
    return true;
}

bool DebugOverlay::CreateTextureResources(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr))
    {
        core::Log::Errorf("Overlay descriptor heap failed: 0x%08X", hr);
        return false;
    }
    m_srvHeap->SetName(L"DebugOverlaySRVHeap");

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kOverlayWidth;
    texDesc.Height = kOverlayHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc = { 1, 0 };
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr, IID_PPV_ARGS(&m_texture));
    if (FAILED(hr))
    {
        core::Log::Errorf("Overlay texture failed: 0x%08X", hr);
        return false;
    }
    m_texture->SetName(L"DebugOverlayTexture");

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = static_cast<UINT64>(m_uploadPitch) * kOverlayHeight;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc = { 1, 0 };
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const wchar_t* uploadNames[kFrameCount] = {
        L"DebugOverlayUpload[0]", L"DebugOverlayUpload[1]", L"DebugOverlayUpload[2]"
    };

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        hr = device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
            &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_uploadBuffers[i]));
        if (FAILED(hr))
        {
            core::Log::Errorf("Overlay upload buffer %u failed: 0x%08X", i, hr);
            return false;
        }
        m_uploadBuffers[i]->SetName(uploadNames[i]);

        D3D12_RANGE readRange = { 0, 0 };
        hr = m_uploadBuffers[i]->Map(0, &readRange,
                                     reinterpret_cast<void**>(&m_uploadMapped[i]));
        if (FAILED(hr) || !m_uploadMapped[i])
        {
            core::Log::Errorf("Overlay upload map %u failed: 0x%08X", i, hr);
            m_uploadMapped[i] = nullptr;
            return false;
        }
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_texture.Get(), &srvDesc,
                                     m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

bool DebugOverlay::CreateTextResources()
{
    m_textDC = CreateCompatibleDC(nullptr);
    if (!m_textDC) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(kOverlayWidth);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(kOverlayHeight);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_textBitmap = CreateDIBSection(m_textDC, &bmi, DIB_RGB_COLORS,
                                    &m_textBits, nullptr, 0);
    if (!m_textBitmap || !m_textBits) return false;

    m_oldBitmap = SelectObject(m_textDC, m_textBitmap);
    SetBkMode(m_textDC, TRANSPARENT);

    m_font = CreateFontA(-15, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                         ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    m_titleFont = CreateFontA(-17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    if (!m_font || !m_titleFont) return false;

    m_oldFont = SelectObject(m_textDC, m_font);
    return true;
}

void DebugOverlay::ReleaseTextResources()
{
    if (m_textDC && m_oldFont)
    {
        SelectObject(m_textDC, m_oldFont);
        m_oldFont = nullptr;
    }
    if (m_textDC && m_oldBitmap)
    {
        SelectObject(m_textDC, m_oldBitmap);
        m_oldBitmap = nullptr;
    }
    if (m_titleFont)
    {
        DeleteObject(m_titleFont);
        m_titleFont = nullptr;
    }
    if (m_font)
    {
        DeleteObject(m_font);
        m_font = nullptr;
    }
    if (m_textBitmap)
    {
        DeleteObject(m_textBitmap);
        m_textBitmap = nullptr;
    }
    if (m_textDC)
    {
        DeleteDC(m_textDC);
        m_textDC = nullptr;
    }
    m_textBits = nullptr;
}

void DebugOverlay::ClearPixels()
{
    std::memset(m_pixels.data(), 0, m_pixels.size());
}

void DebugOverlay::DrawRect(int x, int y, int w, int h, Pixel color)
{
    if (w <= 0 || h <= 0) return;
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = (x + w > static_cast<int>(kOverlayWidth)) ? static_cast<int>(kOverlayWidth) : x + w;
    const int y1 = (y + h > static_cast<int>(kOverlayHeight)) ? static_cast<int>(kOverlayHeight) : y + h;

    for (int py = y0; py < y1; ++py)
    {
        for (int px = x0; px < x1; ++px)
        {
            const size_t idx = (static_cast<size_t>(py) * kOverlayWidth + px) * 4;
            m_pixels[idx + 0] = color.r;
            m_pixels[idx + 1] = color.g;
            m_pixels[idx + 2] = color.b;
            m_pixels[idx + 3] = color.a;
        }
    }
}

void DebugOverlay::DrawTextLine(const char* text, int x, int y, Pixel color, bool title)
{
    if (!text || !m_textDC || !m_textBits) return;

    std::memset(m_textBits, 0, kOverlayWidth * kOverlayHeight * 4);
    SelectObject(m_textDC, title ? m_titleFont : m_font);
    SetTextColor(m_textDC, RGB(255, 255, 255));
    TextOutA(m_textDC, x, y, text, static_cast<int>(std::strlen(text)));

    const uint8_t* mask = static_cast<const uint8_t*>(m_textBits);
    for (uint32_t py = 0; py < kOverlayHeight; ++py)
    {
        for (uint32_t px = 0; px < kOverlayWidth; ++px)
        {
            const size_t idx = (static_cast<size_t>(py) * kOverlayWidth + px) * 4;
            uint8_t coverage = mask[idx + 0];
            if (mask[idx + 1] > coverage) coverage = mask[idx + 1];
            if (mask[idx + 2] > coverage) coverage = mask[idx + 2];
            if (coverage == 0) continue;

            const uint32_t srcA = (static_cast<uint32_t>(coverage) * color.a) / 255u;
            const uint32_t invA = 255u - srcA;

            m_pixels[idx + 0] = static_cast<uint8_t>((color.r * srcA + m_pixels[idx + 0] * invA) / 255u);
            m_pixels[idx + 1] = static_cast<uint8_t>((color.g * srcA + m_pixels[idx + 1] * invA) / 255u);
            m_pixels[idx + 2] = static_cast<uint8_t>((color.b * srcA + m_pixels[idx + 2] * invA) / 255u);

            uint32_t outA = srcA + (static_cast<uint32_t>(m_pixels[idx + 3]) * invA) / 255u;
            if (outA > 255u) outA = 255u;
            m_pixels[idx + 3] = static_cast<uint8_t>(outA);
        }
    }
}

void DebugOverlay::RasterOverlay(const DebugOverlayState& state)
{
    ClearPixels();

    const Pixel panel = { 9, 13, 20, 216 };
    const Pixel panel2 = { 17, 24, 36, 200 };
    const Pixel line = { 123, 144, 174, 130 };
    const Pixel text = { 232, 238, 247, 255 };
    const Pixel muted = { 159, 174, 197, 245 };
    const Pixel accent = state.pathTracing ? Pixel{ 89, 178, 255, 255 } : Pixel{ 118, 222, 172, 255 };
    const Pixel navAccent = { 255, 209, 102, 255 }; // warm amber for the nav heading

    // The panel is only as tall as its content: the engine block always, the
    // navigation block when a star system is active, and the ship-orbit sub-block on
    // top of that when the ship is flown into the system. The unused lower region of
    // the fixed-size texture stays transparent (alpha 0), so the compact engine-only
    // overlay is pixel-identical to before these blocks existed.
    const int usedHeight = !state.navActive ? 176
                         : (state.shipOrbitActive ? static_cast<int>(kOverlayHeight)
                                                  : 252);

    DrawRect(0, 0, kOverlayWidth, kOverlayHeight, { 0, 0, 0, 0 });
    DrawRect(0, 0, kOverlayWidth, usedHeight, panel);
    DrawRect(0, 0, 5, usedHeight, accent);
    DrawRect(0, 31, kOverlayWidth, 1, line);

    char buf[160];
    DrawTextLine("THE DAWNING V3 DEBUG", 16, 7, text, true);

    std::snprintf(buf, sizeof(buf), "FPS %6.1f   FRAME %5.2f MS   ENTITIES %u",
                  state.fps, state.frameMs, state.entityCount);
    DrawTextLine(buf, 16, 41, text);

    std::snprintf(buf, sizeof(buf), "RENDER %-13s  DXR %s",
                  state.pathTracing ? "PATH TRACING" : "RASTERIZED",
                  state.rtAvailable ? "READY" : (state.rtInitAttempted ? "UNAVAILABLE" : "LAZY INIT"));
    DrawTextLine(buf, 16, 61, text);

    if (state.pathTracing)
    {
        std::snprintf(buf, sizeof(buf), "QUALITY %-14s %u SPP / %u BOUNCE%s",
                      state.rtQuality.name,
                      state.rtQuality.samplesPerPixel,
                      state.rtQuality.maxBounces,
                      state.rtQuality.maxBounces == 1 ? "" : "S");
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "QUALITY %-14s F1 STARTS PATH TRACING", "RASTER");
    }
    DrawTextLine(buf, 16, 81, text);

    std::snprintf(buf, sizeof(buf), "CAMERA X %+6.2f  Y %+6.2f  Z %+6.2f   MOUSE %s",
                  state.cameraPosition.x,
                  state.cameraPosition.y,
                  state.cameraPosition.z,
                  state.mouseCaptured ? "CAPTURED" : "FREE");
    DrawTextLine(buf, 16, 101, muted);

    int footerY = 118;
    if (state.navActive)
    {
        // Unit-aware formatting so a moon (hundreds of thousands of km) and a
        // planet (a few AU) are both legible in the same block.
        constexpr double kAU  = 1.495978707e11;
        constexpr double kDay = 86400.0;
        constexpr double kYear = 365.25 * kDay;
        auto fmtLength = [](char* out, size_t n, double m)
        {
            if (std::fabs(m) >= 0.01 * kAU)
                std::snprintf(out, n, "%.4f AU", m / kAU);
            else
                std::snprintf(out, n, "%.0f km", m / 1000.0);
        };
        auto fmtPeriod = [&](char* out, size_t n, double s)
        {
            if (s >= kYear)     std::snprintf(out, n, "%.2f yr", s / kYear);
            else if (s >= kDay) std::snprintf(out, n, "%.1f d", s / kDay);
            else                std::snprintf(out, n, "%.0f s", s);
        };

        DrawRect(0, 118, kOverlayWidth, 1, line); // separator above the nav block
        DrawTextLine("NAVIGATION", 16, 125, navAccent, true);

        int y = 149;
        std::snprintf(buf, sizeof(buf), "VIEW %-10s   WARP %gx",
                      state.cameraModeName, state.timeWarp);
        DrawTextLine(buf, 16, y, text); y += 20;

        char dist[32];
        fmtLength(dist, sizeof(dist), state.focusDistance);
        std::snprintf(buf, sizeof(buf), "FOCUS %-8s   RANGE %s",
                      state.focusName, dist);
        DrawTextLine(buf, 16, y, text); y += 20;

        if (state.focusHasOrbit)
        {
            char sma[32], per[32];
            fmtLength(sma, sizeof(sma), state.focusSemiMajorAxis);
            fmtPeriod(per, sizeof(per), state.focusPeriodSeconds);
            std::snprintf(buf, sizeof(buf), "ORBIT  a %s  e %.4f  T %s",
                          sma, state.focusEccentricity, per);
            DrawTextLine(buf, 16, y, muted); y += 20;
        }
        else
        {
            DrawTextLine("ORBIT  -  central body (frame origin)", 16, y, muted);
            y += 20;
        }

        // Ship-orbit sub-block: the orbital-agency readout.
        if (state.shipOrbitActive)
        {
            const Pixel shipAccent = { 118, 222, 172, 255 }; // green, matches the trace
            y += 6;
            DrawRect(14, y - 4, kOverlayWidth - 28, 1, line);

            char alt[32], spd[32];
            fmtLength(alt, sizeof(alt), state.shipAltitude);
            if (state.shipSpeed >= 1000.0)
                std::snprintf(spd, sizeof(spd), "%.3f km/s", state.shipSpeed / 1000.0);
            else
                std::snprintf(spd, sizeof(spd), "%.0f m/s", state.shipSpeed);
            std::snprintf(buf, sizeof(buf), "SHIP  PRI %-6s  ALT %s  V %s",
                          state.shipPrimaryName, alt, spd);
            DrawTextLine(buf, 16, y, shipAccent, true); y += 20;

            char sma[32];
            fmtLength(sma, sizeof(sma), state.shipSemiMajorAxis);
            std::snprintf(buf, sizeof(buf), "ORBIT  a %s  e %.4f",
                          sma, state.shipEccentricity);
            DrawTextLine(buf, 16, y, text); y += 20;

            char pe[32], ap[32], per[32];
            fmtLength(pe, sizeof(pe), state.shipPeriapsis);
            if (state.shipApoapsis > 0.0)
                fmtLength(ap, sizeof(ap), state.shipApoapsis);
            else
                std::snprintf(ap, sizeof(ap), "%s", "- (escape)");
            if (state.shipPeriodSeconds > 0.0)
                fmtPeriod(per, sizeof(per), state.shipPeriodSeconds);
            else
                std::snprintf(per, sizeof(per), "%s", "-");
            std::snprintf(buf, sizeof(buf), "Pe %s   Ap %s   T %s", pe, ap, per);
            DrawTextLine(buf, 16, y, muted); y += 20;
        }

        // Target sub-block: the HUD lock's range and signed closing speed, coloured
        // by hostility. Matches the target bracket drawn in the world by RenderHud.
        if (state.targetActive)
        {
            const Pixel tgtCol = (state.targetRelation == 2) ? Pixel{ 255,  90,  72, 255 }  // hostile
                               : (state.targetRelation == 1) ? Pixel{ 102, 255, 140, 255 }  // friendly
                                                             : Pixel{ 140, 216, 255, 255 }; // neutral
            y += 6;
            DrawRect(14, y - 4, kOverlayWidth - 28, 1, line);
            char rng[32], cls[32];
            fmtLength(rng, sizeof(rng), state.targetRange);
            if (std::fabs(state.targetClosingSpeed) >= 1000.0)
                std::snprintf(cls, sizeof(cls), "%+.3f km/s", state.targetClosingSpeed / 1000.0);
            else
                std::snprintf(cls, sizeof(cls), "%+.0f m/s", state.targetClosingSpeed);
            std::snprintf(buf, sizeof(buf), "TGT  %-6s  RNG %s", state.targetName, rng);
            DrawTextLine(buf, 16, y, tgtCol, true); y += 20;
            std::snprintf(buf, sizeof(buf), "CLOSING %s", cls);
            DrawTextLine(buf, 16, y, text); y += 20;
        }

        footerY = usedHeight - 40;
    }

    DrawRect(14, footerY, kOverlayWidth - 28, 40, panel2);
    DrawTextLine(state.navActive
                     ? "F4 VIEW   F5 FOCUS   , . / WARP   V ASSIST   F3 HIDE"
                     : "F1 RENDER   F2 QUALITY   F3 OVERLAY   ESC RELEASE/QUIT",
                 24, footerY + 10, muted);
}

void DebugOverlay::UploadTexture(D3D12Device& device)
{
    // Write the slot belonging to this frame in flight. WaitForCurrentFrame() has
    // already retired the frame that last used this slot, so no GPU copy can still
    // be reading it.
    const uint32_t slot = device.FrameIndex();
    uint8_t* mapped = (slot < kFrameCount) ? m_uploadMapped[slot] : nullptr;
    if (!mapped || !m_uploadBuffers[slot]) return;

    for (uint32_t y = 0; y < kOverlayHeight; ++y)
    {
        std::memcpy(mapped + static_cast<size_t>(y) * m_uploadPitch,
                    m_pixels.data() + static_cast<size_t>(y) * kOverlayWidth * 4,
                    kOverlayWidth * 4);
    }

    auto* cmd = device.CmdList();
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = m_uploadBuffers[slot].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = kOverlayWidth;
    src.PlacedFootprint.Footprint.Height = kOverlayHeight;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = m_uploadPitch;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &barrier);
}

void DebugOverlay::Draw(D3D12Device& device, const DebugOverlayState& state)
{
    if (!m_initialized || !state.visible) return;

    RasterOverlay(state);
    UploadTexture(device);

    auto* cmd = device.CmdList();
    auto rtv = device.CurrentRTV();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport = {
        0.0f, 0.0f,
        static_cast<float>(device.Width()),
        static_cast<float>(device.Height()),
        0.0f, 1.0f
    };
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(device.Width()), static_cast<LONG>(device.Height()) };
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    OverlayConstants constants = {};
    constants.viewportSize[0] = static_cast<float>(device.Width());
    constants.viewportSize[1] = static_cast<float>(device.Height());
    constants.overlaySize[0] = static_cast<float>(kOverlayWidth);
    constants.overlaySize[1] = static_cast<float>(kOverlayHeight);
    constants.overlayPos[0] = 14.0f;
    constants.overlayPos[1] = 14.0f;
    constants.opacity = 0.96f;

    cmd->SetGraphicsRoot32BitConstants(0, sizeof(OverlayConstants) / sizeof(uint32_t), &constants, 0);
    cmd->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace render
