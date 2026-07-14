#pragma once
// =============================================================================
// core/window.h — Win32 Window
// =============================================================================
// Creates and manages the game window. Handles:
//   - Window creation and destruction
//   - Message pump (ProcessMessages)
//   - Resize events (signals the renderer to recreate swap chain)
//   - Mouse capture/release for FPS camera control
//   - Raw input for smooth mouse deltas
// =============================================================================

#include <windows.h>
#include <cstdint>

namespace core
{

struct WindowDesc
{
    const char* title = "The Dawning";
    int width = 1920;
    int height = 1080;
    bool startCaptured = false;  // Start with mouse captured
};

class Window
{
public:
    bool Init(const WindowDesc& desc);
    void Shutdown();

    // Pump messages. Returns false if WM_QUIT received.
    bool ProcessMessages();

    // Mouse capture for FPS-style camera
    void CaptureMouse();
    void ReleaseMouse();
    bool IsCaptured() const { return m_captured; }

    // Accessors
    HWND   GetHWND() const { return m_hwnd; }
    int    GetWidth() const { return m_width; }
    int    GetHeight() const { return m_height; }
    float  GetAspectRatio() const { return m_height > 0 ? static_cast<float>(m_width) / static_cast<float>(m_height) : 1.0f; }
    bool   WasResized() const { return m_resized; }
    void   ClearResizeFlag() { m_resized = false; }
    bool   IsMinimized() const { return m_minimized; }
    bool   IsFocused() const { return m_focused; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    HWND     m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    int      m_width = 0;
    int      m_height = 0;
    bool     m_resized = false;
    bool     m_minimized = false;
    bool     m_captured = false;
    bool     m_focused = true;
    bool     m_running = true;
};

} // namespace core
