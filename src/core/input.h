#pragma once
// =============================================================================
// core/input.h — Input System
// =============================================================================
// Tracks keyboard key states (pressed this frame, held, released this frame)
// and mouse position, delta, buttons, and wheel.
// Call BeginFrame() at the start of each frame before processing messages.
// Call ProcessKeyDown/Up/MouseMove/etc from the WndProc.
// =============================================================================

#include <cstdint>

namespace core { namespace input {

struct MouseState
{
    int  x = 0;                // Current screen position
    int  y = 0;
    int  deltaX = 0;           // Movement this frame
    int  deltaY = 0;
    int  wheelDelta = 0;       // Scroll wheel this frame
    bool buttons[3] = {};      // Left, Right, Middle
    bool buttonPressed[3] = {};
    bool buttonReleased[3] = {};
};

struct KeyboardState
{
    bool keys[256] = {};            // Currently held
    bool keyPressed[256] = {};      // Pressed this frame
    bool keyReleased[256] = {};     // Released this frame
};

struct InputState
{
    MouseState    mouse;
    KeyboardState keyboard;

    bool KeyDown(int vk) const     { return keyboard.keys[vk & 0xFF]; }
    bool KeyPressed(int vk) const  { return keyboard.keyPressed[vk & 0xFF]; }
    bool KeyReleased(int vk) const { return keyboard.keyReleased[vk & 0xFF]; }
};

// Call once at start of frame to clear per-frame deltas
void BeginFrame();

// Call from WndProc
void ProcessKeyDown(uint32_t vkCode);
void ProcessKeyUp(uint32_t vkCode);
void ProcessMouseMove(int x, int y);
void ProcessMouseButton(int button, bool down);  // 0=left, 1=right, 2=middle
void ProcessMouseWheel(int delta);
void ProcessRawMouseDelta(int dx, int dy);

// Flush all key/button state (call on focus loss to prevent stuck keys)
void FlushAll();

// Get current state (read-only snapshot valid for this frame)
const InputState& GetState();

}} // namespace core::input
