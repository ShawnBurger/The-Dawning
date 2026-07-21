// =============================================================================
// core/timer.cpp — Frame Timer Implementation
// =============================================================================

#include "timer.h"
#include <windows.h>
#include <cmath>
#include <cstdint>

namespace core
{

void Timer::Init()
{
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);

    m_frequency = freq.QuadPart;
    m_lastTime  = now.QuadPart;
    m_startTime = now.QuadPart;
    m_totalTime = 0.0;
    m_frameCount = 0;
    m_accumulator = 0.0;
    m_fpsAccum = 0.0;
    m_fpsFrames = 0;
    m_fps = 0.0f;
}

TimeStep Timer::Tick()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    int64_t elapsed = now.QuadPart - m_lastTime;
    m_lastTime = now.QuadPart;

    double dt = static_cast<double>(elapsed) / static_cast<double>(m_frequency);

    // Clamp to prevent spiral of death (e.g., breakpoint, window drag)
    if (dt > kMaxDt) dt = kMaxDt;
    if (dt < 0.0)    dt = 0.0;

    m_totalTime += dt;
    m_frameCount++;

    // Feed fixed timestep accumulator
    m_accumulator += dt;

    // Smoothed FPS (update every 0.5 seconds)
    m_fpsAccum += dt;
    m_fpsFrames++;
    if (m_fpsAccum >= 0.5)
    {
        m_fps = static_cast<float>(m_fpsFrames / m_fpsAccum);
        m_fpsAccum = 0.0;
        m_fpsFrames = 0;
    }

    TimeStep ts;
    ts.dt = dt;
    ts.totalTime = m_totalTime;
    ts.frameCount = m_frameCount;
    ts.fps = m_fps;
    return ts;
}

bool Timer::ConsumeFixedStep()
{
    if (m_accumulator >= m_fixedDt)
    {
        m_accumulator -= m_fixedDt;
        return true;
    }
    return false;
}

bool Timer::SetFixedDt(double dt)
{
    if (!(dt > 0.0) || !std::isfinite(dt))
        return false;
    m_fixedDt = dt;
    return true;
}

} // namespace core
