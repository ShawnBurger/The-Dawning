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
    core::Vec3f cameraPosition = {};
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
    static constexpr uint32_t kOverlayHeight = 176;

    bool m_initialized = false;
    uint32_t m_uploadPitch = 0;
    std::vector<uint8_t> m_pixels;

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    ComPtr<ID3D12Resource> m_texture;
    ComPtr<ID3D12Resource> m_uploadBuffer;
    uint8_t* m_uploadMapped = nullptr;

    HDC m_textDC = nullptr;
    HBITMAP m_textBitmap = nullptr;
    void* m_textBits = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
    HFONT m_font = nullptr;
    HFONT m_titleFont = nullptr;
    HGDIOBJ m_oldFont = nullptr;
};

} // namespace render
