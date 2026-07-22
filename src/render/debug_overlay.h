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
    // Tall enough for the navigation block. When navActive is false the lower
    // region is left transparent (the panel background is drawn only as tall as
    // the content), so the compact engine-only overlay is unchanged in look.
    static constexpr uint32_t kOverlayHeight = 288;

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
