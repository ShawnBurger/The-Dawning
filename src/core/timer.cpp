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

    // Feed the fixed-step accumulator with this frame's WALL time, scaled by the
    // time-acceleration factor (and capped against a spiral of death).
    AdvanceTime(dt);

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

bool Timer::SetTimeScale(double scale)
{
    if (!(scale > 0.0) || !std::isfinite(scale))
        return false;
    m_timeScale = scale;
    return true;
}

void Timer::AdvanceTime(double realDt)
{
    if (!std::isfinite(realDt) || realDt < 0.0)
        return;
    m_accumulator += realDt * m_timeScale;
    // Cap accumulated physics work per frame so a large scale (or long real frame)
    // cannot demand unbounded steps and freeze the loop; excess wall time is
    // dropped, so the sim runs at min(requested, cap)x rather than stalling.
    const double maxAccum = static_cast<double>(kMaxStepsPerTick) * m_fixedDt;
    if (m_accumulator > maxAccum)
        m_accumulator = maxAccum;
}

} // namespace core
