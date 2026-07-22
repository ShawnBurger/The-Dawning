#pragma once
// =============================================================================
// render/hud.h — self-contained screen-space 2D HUD renderer
// =============================================================================
// The gameplay HUD (targeting, flight, docking, ship-status) is built from 2D
// primitives in SCREEN PIXELS. This module owns its own root signature, PSO and
// per-frame dynamic vertex buffer, so it adds a HUD layer without touching the
// shared Renderer/scene render code — the app builds a primitive list each frame
// via the immediate-mode helpers below and calls Draw() into the already-bound HDR
// target, at the same stage as the orbit-line / billboard markers.
//
// Everything is emitted as TRIANGLES (lines are thin quads) so one pipeline draws
// the whole HUD and every primitive supports a pixel thickness. World-anchored
// elements are projected to pixels on the CPU (WorldToScreen) before being added.
// =============================================================================

#include "core/types.h"

#include <cstdint>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

namespace render
{

class D3D12Device;

// Project a CAMERA-RELATIVE, render-scaled world point (RULE 1: caller already
// subtracted the camera in double and applied K) through the camera-relative
// viewProj to screen pixels. Returns false if the point is at/behind the camera
// plane (onScreen also false when the projected pixel is outside the viewport).
struct ScreenPoint { float x, y; bool visible; bool onScreen; };
ScreenPoint WorldToScreen(const core::Mat4x4& viewProj, const core::Vec3f& cameraRelPos,
                          float viewportW, float viewportH);

class HudRenderer
{
public:
    // Create root signature, PSO and the per-frame vertex buffers. Non-fatal on
    // failure (the caller checks IsReady()); the HUD simply does not draw.
    bool Init(ID3D12Device* device);
    bool IsReady() const { return m_ready; }

    // --- Immediate-mode frame API -------------------------------------------
    // Begin() clears the accumulated primitives; the Add* helpers append screen-
    // space triangles; Draw() uploads them and records the draw into the bound RTV.
    void Begin();

    // A filled axis-aligned rectangle (x,y = top-left, in pixels).
    void AddRect(float x, float y, float w, float h, const core::Color& c);
    // A line segment of the given pixel thickness (a thin quad).
    void AddLine(float x0, float y0, float x1, float y1, const core::Color& c,
                 float thickness = 2.0f);
    // A rectangle OUTLINE (four thickness-wide edges).
    void AddRectOutline(float x, float y, float w, float h, const core::Color& c,
                        float thickness = 2.0f);
    // A target BRACKET: four L-shaped corners of a square of half-extent `half`
    // centred at (cx,cy); `corner` is the arm length of each L.
    void AddBracket(float cx, float cy, float half, float corner, const core::Color& c,
                    float thickness = 2.0f);
    // A crosshair RETICLE centred at (cx,cy): four ticks with a central gap.
    void AddReticle(float cx, float cy, float radius, float gap, const core::Color& c,
                    float thickness = 2.0f);
    // A ring (circle outline) of `segments` segments.
    void AddCircle(float cx, float cy, float radius, const core::Color& c,
                   float thickness = 2.0f, int segments = 32);
    // A filled diamond (rotated square) marker — velocity/lead markers.
    void AddDiamond(float cx, float cy, float radius, const core::Color& c);
    // A diamond OUTLINE.
    void AddDiamondOutline(float cx, float cy, float radius, const core::Color& c,
                           float thickness = 2.0f);
    // A chevron (>) pointing toward `angleRad` — off-screen target arrows.
    void AddChevron(float cx, float cy, float size, float angleRad,
                    const core::Color& c, float thickness = 3.0f);
    // A filled triangle (three pixel corners).
    void AddTri(float x0, float y0, float x1, float y1, float x2, float y2,
                const core::Color& c);

    // Upload the accumulated primitives and draw them into the currently-bound HDR
    // RTV (call after the scene + markers, before tone-map). No-op if empty or the
    // renderer failed to initialise. `frameIndex` selects the in-flight VB slot.
    void Draw(D3D12Device& device, float viewportW, float viewportH, uint64_t frameIndex);

private:
    struct HudVertex { float x, y; float r, g, b, a; };

    static constexpr uint32_t kFrameCount   = 3;       // matches the swap chain
    static constexpr uint32_t kMaxVertices  = 49152;   // ~8k primitives, clamped

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_vb[kFrameCount];
    HudVertex* m_mapped[kFrameCount] = {};
    std::vector<HudVertex> m_verts;
    bool m_ready = false;
    bool m_overflowLogged = false;
};

} // namespace render
