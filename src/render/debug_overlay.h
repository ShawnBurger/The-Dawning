#pragma once
// =============================================================================
// render/debug_overlay.h - lightweight D3D12 debug overlay
// =============================================================================

#include "d3d12_device.h"
#include "path_tracer.h"
#include "../core/types.h"
#include <cstdint>
#include <vector>

namespace render
{

struct DebugOverlayState
{
    bool visible = true;
    bool pathTracing = false;
    bool rtAvailable = false;
    bool rtInitAttempted = false;
    bool mouseCaptured = false;
    float fps = 0.0f;
    double frameMs = 0.0;
    uint32_t entityCount = 0;
    RTQualityInfo rtQuality = {};
    core::Vec3d cameraPosition = {};

    // -------------------------------------------------------------------------
    // Navigation block — drawn only when a star system is active. Makes the
    // simulation legible: which view/body the camera is on, the time-warp rate,
    // and the focused body's live orbit. The string pointers are borrowed from
    // static tables in app.cpp and outlive the copy, so no ownership is taken.
    // -------------------------------------------------------------------------
    bool         navActive = false;
    const char*  cameraModeName = "";
    double       timeWarp = 1.0;
    const char*  focusName = "";
    double       focusDistance = 0.0;    // metres, camera -> focused body centre
    bool         focusHasOrbit = false;  // false for the star / bodies with no orbit
    double       focusSemiMajorAxis = 0.0; // a (m); < 0 for hyperbolic
    double       focusEccentricity = 0.0;  // e
    double       focusPeriodSeconds = 0.0; // 0 when not a closed ellipse

    // Ship orbit sub-block — the player ship's OWN live osculating orbit about its
    // SOI primary, shown when it has been flown into the system. This is the
    // orbital-agency readout: a prograde burn visibly raises the apoapsis here.
    bool         shipOrbitActive = false;
    const char*  shipPrimaryName = "";
    double       shipAltitude = 0.0;      // range from primary centre (m)
    double       shipSpeed = 0.0;         // speed relative to primary (m/s)
    double       shipSemiMajorAxis = 0.0; // a (m)
    double       shipEccentricity = 0.0;  // e
    double       shipPeriapsis = 0.0;     // r_min (m from primary centre)
    double       shipApoapsis = 0.0;      // r_max (m); 0 if not elliptic
    double       shipPeriodSeconds = 0.0; // 0 if not a closed ellipse

    // Target sub-block — the HUD's locked target (T cycles, G clears). The range and
    // signed closing speed the targeting reticle/bracket is drawn around. relation:
    // 0 neutral, 1 friendly, 2 hostile (drives the label colour).
    bool         targetActive = false;
    const char*  targetName = "";
    double       targetRange = 0.0;         // m, ship -> target centre
    double       targetClosingSpeed = 0.0;  // m/s, positive = closing
    int          targetRelation = 0;

    // Flight sub-block — the ship-status readout mirroring the HUD's speed bar and
    // thrust indicator: coupled/decoupled mode, forward throttle, felt G, rotation.
    bool         flightActive = false;
    bool         flightCoupled = true;
    float        flightThrottleFwd = 0.0f;  // -1..1
    float        flightGForce = 0.0f;
    float        flightRotRateDeg = 0.0f;

    // Docking sub-block — the ILS-style guidance for the dockable platform, mirroring
    // the HUD widget: the dock state, range to the port, closing speed vs the governor
    // limit, and the attitude errors in degrees. Shown only near the platform.
    bool         dockActive = false;
    const char*  dockStateName = "";
    int          dockStateCode = 0;         // 0 Idle,1 Approach,2 Align,3 Hold,4 Docked,5 Undock
    double       dockRange = 0.0;           // m, ship -> port
    double       dockClosingSpeed = 0.0;    // m/s toward the port (+ = closing)
    double       dockMaxSpeed = 0.0;        // m/s, governor limit at this distance
    bool         dockOverspeed = false;
    bool         dockInCorridor = false;
    float        dockAlignErrorDeg = 0.0f;  // nose vs port axis
    float        dockRollErrorDeg = 0.0f;   // roll vs port up
    float        dockLateral = 0.0f;        // m off the centreline
};

class DebugOverlay
{
public:
    bool Init(D3D12Device& device);
    void Shutdown();

    // Back buffer must be in RENDER_TARGET state when this is called.
    void Draw(D3D12Device& device, const DebugOverlayState& state);

private:
    struct Pixel
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    bool CreatePipeline(ID3D12Device* device);
    bool CreateTextureResources(ID3D12Device* device);
    bool CreateTextResources();
    void ReleaseTextResources();

    void RasterOverlay(const DebugOverlayState& state);
    void ClearPixels();
    void DrawRect(int x, int y, int w, int h, Pixel color);
    void DrawTextLine(const char* text, int x, int y, Pixel color, bool title = false);
    void UploadTexture(D3D12Device& device);

    static constexpr uint32_t kOverlayWidth = 520;
    // Tall enough for the navigation block plus every optional sub-block stacked at
    // once: ship-orbit, target, flight AND docking, then the footer. RasterOverlay sizes
    // the drawn panel to the actual active content (usedHeight), so the unused lower
    // region stays transparent and the compact engine-only overlay is unchanged in look.
    static constexpr uint32_t kOverlayHeight = 512;

    bool m_initialized = false;
    uint32_t m_uploadPitch = 0;
    std::vector<uint8_t> m_pixels;

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    ComPtr<ID3D12Resource> m_texture;

    // One upload buffer per frame in flight. A single shared buffer would be
    // rewritten by the CPU while earlier frames' CopyTextureRegion still read it:
    // resource barriers order GPU work, they do not synchronise CPU writes to
    // persistently mapped memory, so no barrier can protect this.
    ComPtr<ID3D12Resource> m_uploadBuffers[kFrameCount];
    uint8_t* m_uploadMapped[kFrameCount] = {};

    HDC m_textDC = nullptr;
    HBITMAP m_textBitmap = nullptr;
    void* m_textBits = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    HFONT m_font = nullptr;
    HFONT m_titleFont = nullptr;
    HGDIOBJ m_oldFont = nullptr;
};

} // namespace render
