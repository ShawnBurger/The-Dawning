// =============================================================================
// core/input.cpp — Input System Implementation
// =============================================================================

#include "input.h"
#include <cstring>
#include <cstdint>

namespace core { namespace input {

static InputState s_state;

void BeginFrame()
{
    // Clear per-frame events
    s_state.mouse.deltaX = 0;
    s_state.mouse.deltaY = 0;
    s_state.mouse.wheelDelta = 0;

    for (int i = 0; i < 3; i++)
    {
        s_state.mouse.buttonPressed[i] = false;
        s_state.mouse.buttonReleased[i] = false;
    }

    for (int i = 0; i < 256; i++)
    {
        s_state.keyboard.keyPressed[i] = false;
        s_state.keyboard.keyReleased[i] = false;
    }
}

void ProcessKeyDown(uint32_t vkCode)
{
    uint8_t k = static_cast<uint8_t>(vkCode & 0xFF);
    if (!s_state.keyboard.keys[k])
        s_state.keyboard.keyPressed[k] = true;
    s_state.keyboard.keys[k] = true;
}

void ProcessKeyUp(uint32_t vkCode)
{
    uint8_t k = static_cast<uint8_t>(vkCode & 0xFF);
    s_state.keyboard.keys[k] = false;
    s_state.keyboard.keyReleased[k] = true;
}

void ProcessMouseMove(int x, int y)
{
    s_state.mouse.x = x;
    s_state.mouse.y = y;
}

void ProcessMouseButton(int button, bool down)
{
    if (button < 0 || button > 2) return;
    if (down && !s_state.mouse.buttons[button])
        s_state.mouse.buttonPressed[button] = true;
    if (!down && s_state.mouse.buttons[button])
        s_state.mouse.buttonReleased[button] = true;
    s_state.mouse.buttons[button] = down;
}

void ProcessMouseWheel(int delta)
{
    s_state.mouse.wheelDelta += delta;
}

void ProcessRawMouseDelta(int dx, int dy)
{
    s_state.mouse.deltaX += dx;
    s_state.mouse.deltaY += dy;
}

void FlushAll()
{
    // Clear all key and button state — prevents stuck keys on focus loss
    for (int i = 0; i < 256; i++)
    {
        s_state.keyboard.keys[i] = false;
        s_state.keyboard.keyPressed[i] = false;
        s_state.keyboard.keyReleased[i] = false;
    }
    for (int i = 0; i < 3; i++)
    {
        s_state.mouse.buttons[i] = false;
        s_state.mouse.buttonPressed[i] = false;
        s_state.mouse.buttonReleased[i] = false;
    }
    s_state.mouse.deltaX = 0;
    s_state.mouse.deltaY = 0;
    s_state.mouse.wheelDelta = 0;
}

const InputState& GetState()
{
    return s_state;
}

}} // namespace core::input
