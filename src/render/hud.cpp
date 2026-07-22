// =============================================================================
// render/hud.cpp — see hud.h
// =============================================================================
#include "hud.h"

#include "d3d12_device.h"
#include "shader_utils.h"
#include "core/log.h"

#include <cmath>
#include <cstring>

namespace render
{

ScreenPoint WorldToScreen(const core::Mat4x4& vp, const core::Vec3f& p,
                          float viewportW, float viewportH)
{
    // clip = vp applied to (p,1), matching the GPU mul(g_viewProj, float4(p,1)) —
    // the CPU stores row-major, so clip.i = sum_j p_j * m[j][i] (see TransformPoint).
    const float cx = p.x * vp.m[0][0] + p.y * vp.m[1][0] + p.z * vp.m[2][0] + vp.m[3][0];
    const float cy = p.x * vp.m[0][1] + p.y * vp.m[1][1] + p.z * vp.m[2][1] + vp.m[3][1];
    const float cw = p.x * vp.m[0][3] + p.y * vp.m[1][3] + p.z * vp.m[2][3] + vp.m[3][3];

    ScreenPoint sp{ 0.0f, 0.0f, false, false };
    if (cw <= 1e-6f) return sp; // at or behind the camera plane

    const float ndcx = cx / cw;
    const float ndcy = cy / cw;
    sp.x = (ndcx * 0.5f + 0.5f) * viewportW;
    sp.y = (1.0f - (ndcy * 0.5f + 0.5f)) * viewportH; // screen y is down
    sp.visible  = true;
    sp.onScreen = (sp.x >= 0.0f && sp.x <= viewportW && sp.y >= 0.0f && sp.y <= viewportH);
    return sp;
}

bool HudRenderer::Init(ID3D12Device* device)
{
    auto vs = CompileShaderFromFile(L"shaders/hud_vs.hlsl", "main", "vs_5_1");
    auto ps = CompileShaderFromFile(L"shaders/hud_ps.hlsl", "main", "ps_5_1");
    if (!vs || !ps)
    {
        core::Log::Error("HUD shader compile failed; HUD disabled");
        return false;
    }

    // Root signature: one root-constant block (viewport float2 + pad) at b0, visible
    // to the vertex stage. Input-assembler layout enabled for the vertex buffer.
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.ShaderRegister = 0;
    param.Constants.RegisterSpace  = 0;
    param.Constants.Num32BitValues = 4; // viewport.xy + pad.xy
    param.ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 1;
    rs.pParameters   = &param;
    rs.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr))
    {
        if (err) core::Log::Errorf("HUD root sig: %s", static_cast<const char*>(err->GetBufferPointer()));
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                           IID_PPV_ARGS(&m_rootSig))))
        return false;
    m_rootSig->SetName(L"HudRootSig");

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_rootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout           = { layout, 2 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    // Straight alpha blend so the HUD composites over the scene.
    pso.BlendState.RenderTarget[0].BlendEnable    = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0]    = DXGI_FORMAT_R16G16B16A16_FLOAT; // the linear HDR scene target
    pso.SampleDesc       = { 1, 0 };
    pso.SampleMask       = UINT_MAX;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso))))
    {
        core::Log::Error("HUD PSO creation failed; HUD disabled");
        return false;
    }
    m_pso->SetName(L"HudPSO");

    // Per-frame dynamic vertex buffers (persistently mapped upload heap).
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC vbDesc = {};
    vbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    vbDesc.Width            = static_cast<UINT64>(kMaxVertices) * sizeof(HudVertex);
    vbDesc.Height           = 1;
    vbDesc.DepthOrArraySize = 1;
    vbDesc.MipLevels        = 1;
    vbDesc.SampleDesc       = { 1, 0 };
    vbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (FAILED(device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vb[i]))))
            return false;
        m_vb[i]->SetName(L"HudVertexBuffer");
        D3D12_RANGE noRead = { 0, 0 };
        if (FAILED(m_vb[i]->Map(0, &noRead, reinterpret_cast<void**>(&m_mapped[i]))))
            return false;
    }

    m_verts.reserve(4096);
    m_ready = true;
    return true;
}

void HudRenderer::Begin() { m_verts.clear(); }

// --- primitive builders ------------------------------------------------------
void HudRenderer::AddTri(float x0, float y0, float x1, float y1, float x2, float y2,
                         const core::Color& c)
{
    m_verts.push_back({ x0, y0, c.r, c.g, c.b, c.a });
    m_verts.push_back({ x1, y1, c.r, c.g, c.b, c.a });
    m_verts.push_back({ x2, y2, c.r, c.g, c.b, c.a });
}

void HudRenderer::AddRect(float x, float y, float w, float h, const core::Color& c)
{
    AddTri(x, y, x + w, y, x + w, y + h, c);
    AddTri(x, y, x + w, y + h, x, y + h, c);
}

void HudRenderer::AddLine(float x0, float y0, float x1, float y1, const core::Color& c,
                          float thickness)
{
    float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    dx /= len; dy /= len;
    const float hx = -dy * thickness * 0.5f; // perpendicular * half-thickness
    const float hy =  dx * thickness * 0.5f;
    AddTri(x0 + hx, y0 + hy, x1 + hx, y1 + hy, x1 - hx, y1 - hy, c);
    AddTri(x0 + hx, y0 + hy, x1 - hx, y1 - hy, x0 - hx, y0 - hy, c);
}

void HudRenderer::AddRectOutline(float x, float y, float w, float h, const core::Color& c,
                                 float thickness)
{
    AddLine(x, y, x + w, y, c, thickness);
    AddLine(x + w, y, x + w, y + h, c, thickness);
    AddLine(x + w, y + h, x, y + h, c, thickness);
    AddLine(x, y + h, x, y, c, thickness);
}

void HudRenderer::AddBracket(float cx, float cy, float half, float corner,
                             const core::Color& c, float thickness)
{
    const float l = cx - half, r = cx + half, t = cy - half, b = cy + half;
    // Top-left, top-right, bottom-right, bottom-left L corners.
    AddLine(l, t, l + corner, t, c, thickness); AddLine(l, t, l, t + corner, c, thickness);
    AddLine(r, t, r - corner, t, c, thickness); AddLine(r, t, r, t + corner, c, thickness);
    AddLine(r, b, r - corner, b, c, thickness); AddLine(r, b, r, b - corner, c, thickness);
    AddLine(l, b, l + corner, b, c, thickness); AddLine(l, b, l, b - corner, c, thickness);
}

void HudRenderer::AddReticle(float cx, float cy, float radius, float gap,
                             const core::Color& c, float thickness)
{
    AddLine(cx, cy - radius, cx, cy - gap, c, thickness);
    AddLine(cx, cy + gap, cx, cy + radius, c, thickness);
    AddLine(cx - radius, cy, cx - gap, cy, c, thickness);
    AddLine(cx + gap, cy, cx + radius, cy, c, thickness);
}

void HudRenderer::AddCircle(float cx, float cy, float radius, const core::Color& c,
                            float thickness, int segments)
{
    if (segments < 3) segments = 3;
    const float step = 6.2831853f / static_cast<float>(segments);
    float px = cx + radius, py = cy;
    for (int i = 1; i <= segments; ++i)
    {
        const float a = step * static_cast<float>(i);
        const float nx = cx + radius * std::cos(a);
        const float ny = cy + radius * std::sin(a);
        AddLine(px, py, nx, ny, c, thickness);
        px = nx; py = ny;
    }
}

void HudRenderer::AddDiamond(float cx, float cy, float radius, const core::Color& c)
{
    AddTri(cx, cy - radius, cx + radius, cy, cx, cy + radius, c);
    AddTri(cx, cy - radius, cx, cy + radius, cx - radius, cy, c);
}

void HudRenderer::AddDiamondOutline(float cx, float cy, float radius, const core::Color& c,
                                    float thickness)
{
    AddLine(cx, cy - radius, cx + radius, cy, c, thickness);
    AddLine(cx + radius, cy, cx, cy + radius, c, thickness);
    AddLine(cx, cy + radius, cx - radius, cy, c, thickness);
    AddLine(cx - radius, cy, cx, cy - radius, c, thickness);
}

void HudRenderer::AddChevron(float cx, float cy, float size, float angleRad,
                             const core::Color& c, float thickness)
{
    // A ">" whose point faces `angleRad`. Tip at cx,cy + dir*size; the two arms
    // trail back at +/-135 degrees from the pointing direction.
    const float ca = std::cos(angleRad), sa = std::sin(angleRad);
    const float tipx = cx + ca * size, tipy = cy + sa * size;
    const float a1 = angleRad + 2.3561945f; // +135 deg
    const float a2 = angleRad - 2.3561945f; // -135 deg
    AddLine(tipx, tipy, tipx + std::cos(a1) * size, tipy + std::sin(a1) * size, c, thickness);
    AddLine(tipx, tipy, tipx + std::cos(a2) * size, tipy + std::sin(a2) * size, c, thickness);
}

void HudRenderer::Draw(D3D12Device& device, float viewportW, float viewportH,
                       uint64_t frameIndex)
{
    if (!m_ready || m_verts.empty()) return;

    uint32_t count = static_cast<uint32_t>(m_verts.size());
    if (count > kMaxVertices)
    {
        if (!m_overflowLogged)
        {
            core::Log::Warnf("HUD vertex overflow: %u > %u; clamping", count, kMaxVertices);
            m_overflowLogged = true;
        }
        count = kMaxVertices;
    }

    const uint32_t slot = static_cast<uint32_t>(frameIndex % kFrameCount);
    std::memcpy(m_mapped[slot], m_verts.data(), static_cast<size_t>(count) * sizeof(HudVertex));

    ID3D12GraphicsCommandList* cmd = device.CmdList();
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_pso.Get());
    const float vp[4] = { viewportW, viewportH, 0.0f, 0.0f };
    cmd->SetGraphicsRoot32BitConstants(0, 4, vp, 0);

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = m_vb[slot]->GetGPUVirtualAddress();
    vbv.SizeInBytes    = count * sizeof(HudVertex);
    vbv.StrideInBytes  = sizeof(HudVertex);
    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(count, 1, 0, 0);
}

} // namespace render
