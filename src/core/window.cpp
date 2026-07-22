// =============================================================================
// core/window.cpp — Win32 Window Implementation
// =============================================================================

#include "window.h"
#include "log.h"
#include "input.h"
#include <cstdint>

namespace core
{

bool Window::Init(const WindowDesc& desc)
{
    m_hInstance = GetModuleHandle(nullptr);
    m_width = desc.width;
    m_height = desc.height;

    // Register window class
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = m_hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = "TheDawningV3WindowClass";
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExA(&wc))
    {
        Log::Error("Failed to register window class");
        return false;
    }

    // Calculate window size to achieve desired client area
    RECT rect = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rect, style, FALSE);

    int windowWidth  = rect.right - rect.left;
    int windowHeight = rect.bottom - rect.top;

    // Center on screen
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - windowWidth) / 2;
    int posY = (screenH - windowHeight) / 2;

    m_hwnd = CreateWindowExA(
        0,
        "TheDawningV3WindowClass",
        desc.title,
        style,
        posX, posY, windowWidth, windowHeight,
        nullptr, nullptr, m_hInstance, this  // Pass 'this' via lpParam
    );

    if (!m_hwnd)
    {
        Log::Error("Failed to create window");
        return false;
    }

    // Register for raw mouse input (smooth deltas, no acceleration)
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;  // Generic desktop controls
    rid.usUsage     = 0x02;  // Mouse
    rid.dwFlags     = 0;
    rid.hwndTarget  = m_hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
    {
        // Not fatal - the window still works - but mouse look is dead without it,
        // and silently losing camera control is a confusing way to find out.
        Log::Warnf("RegisterRawInputDevices failed (error %lu); mouse look disabled",
                   GetLastError());
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    if (desc.startCaptured)
        CaptureMouse();

    Log::Infof("Window created: %dx%d", m_width, m_height);
    return true;
}

void Window::Shutdown()
{
    if (m_captured)
        ReleaseMouse();
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    UnregisterClassA("TheDawningV3WindowClass", m_hInstance);
    Log::Info("Window destroyed");
}

bool Window::ProcessMessages()
{
    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            m_running = false;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return m_running;
}

void Window::CaptureMouse()
{
    if (m_captured) return;
    SetCapture(m_hwnd);
    ShowCursor(FALSE);

    // Clip cursor to window client area
    RECT clipRect;
    GetClientRect(m_hwnd, &clipRect);
    POINT tl = { clipRect.left, clipRect.top };
    POINT br = { clipRect.right, clipRect.bottom };
    ClientToScreen(m_hwnd, &tl);
    ClientToScreen(m_hwnd, &br);
    clipRect = { tl.x, tl.y, br.x, br.y };
    ClipCursor(&clipRect);

    // Center cursor
    POINT center = { (br.x - tl.x) / 2 + tl.x, (br.y - tl.y) / 2 + tl.y };
    SetCursorPos(center.x, center.y);

    m_captured = true;
}

void Window::ReleaseMouse()
{
    if (!m_captured) return;
    ClipCursor(nullptr);  // Remove cursor clipping
    ::ReleaseCapture();
    ShowCursor(TRUE);
    m_captured = false;
}

// =============================================================================
// Static WndProc — routes to instance method
// =============================================================================
LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Window* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        self = cs ? reinterpret_cast<Window*>(cs->lpCreateParams) : nullptr;
        if (self)
        {
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->m_hwnd = hwnd;
        }
        // A null lpCreateParams means someone created this class's window without
        // passing the instance. Fall through to DefWindowProc rather than
        // dereferencing null inside the window procedure.
    }
    else
    {
        self = reinterpret_cast<Window*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }

    if (self)
        return self->HandleMessage(msg, wParam, lParam);

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT Window::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        m_running = false;
        return 0;

    case WM_SIZE:
    {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        m_minimized = (wParam == SIZE_MINIMIZED);
        if (w > 0 && h > 0 && (w != m_width || h != m_height))
        {
            m_width = w;
            m_height = h;
            m_resized = true;

            // Refresh cursor clip rect if mouse is captured
            if (m_captured)
            {
                RECT clipRect;
                GetClientRect(m_hwnd, &clipRect);
                POINT tl = { clipRect.left, clipRect.top };
                POINT br = { clipRect.right, clipRect.bottom };
                ClientToScreen(m_hwnd, &tl);
                ClientToScreen(m_hwnd, &br);
                clipRect = { tl.x, tl.y, br.x, br.y };
                ClipCursor(&clipRect);
            }

            Log::Infof("Window resized: %dx%d", m_width, m_height);
        }
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        input::ProcessKeyDown(static_cast<uint32_t>(wParam));
        // Alt+Enter for fullscreen toggle could go here
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        input::ProcessKeyUp(static_cast<uint32_t>(wParam));
        return 0;

    case WM_MOUSEMOVE:
    {
        int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        input::ProcessMouseMove(x, y);
        return 0;
    }

    case WM_LBUTTONDOWN: input::ProcessMouseButton(0, true);  return 0;
    case WM_LBUTTONUP:   input::ProcessMouseButton(0, false); return 0;
    case WM_RBUTTONDOWN: input::ProcessMouseButton(1, true);  return 0;
    case WM_RBUTTONUP:   input::ProcessMouseButton(1, false); return 0;
    case WM_MBUTTONDOWN: input::ProcessMouseButton(2, true);  return 0;
    case WM_MBUTTONUP:   input::ProcessMouseButton(2, false); return 0;

    case WM_MOUSEWHEEL:
        input::ProcessMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA);
        return 0;

    case WM_INPUT:
    {
        UINT size = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size > 0 && size <= 256)
        {
            alignas(RAWINPUT) uint8_t buffer[256]; // RAWINPUT has 8-byte-aligned members
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == size)
            {
                auto* raw = reinterpret_cast<RAWINPUT*>(buffer);
                if (raw->header.dwType == RIM_TYPEMOUSE)
                {
                    input::ProcessRawMouseDelta(raw->data.mouse.lLastX, raw->data.mouse.lLastY);
                }
            }
        }
        // WM_INPUT requires DefWindowProc for system cleanup of the raw-input data.
        return DefWindowProcA(m_hwnd, msg, wParam, lParam);
    }

    // Focus management — prevent stuck keys, release mouse on alt-tab
    case WM_KILLFOCUS:
        m_focused = false;
        input::FlushAll();  // Prevent stuck keys when alt-tabbing
        if (m_captured)
            ReleaseMouse();
        Log::Info("Window lost focus");
        return 0;

    case WM_SETFOCUS:
        m_focused = true;
        Log::Info("Window gained focus");
        return 0;

    // Prevent Alt key from activating the menu bar
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU)
            return 0;
        break;
    }

    return DefWindowProcA(m_hwnd, msg, wParam, lParam);
}

} // namespace core
