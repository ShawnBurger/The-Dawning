#pragma once
// =============================================================================
// core/timer.h — High-Resolution Frame Timer
// =============================================================================
// Uses QueryPerformanceCounter for sub-microsecond precision.
// Provides variable dt for rendering and a fixed timestep accumulator for physics.
// =============================================================================

#include <cstdint>

namespace core
{

struct TimeStep
{
    double dt;          // Seconds since last frame (variable, for rendering)
    double totalTime;   // Total elapsed seconds since Init()
    uint64_t frameCount;
    float fps;          // Smoothed frames per second
};

class Timer
{
public:
    void Init();

    // Call once per frame. Returns the timestep.
    TimeStep Tick();

    // Fixed timestep accumulator for physics
    // Returns true while there's a pending fixed step to consume.
    // Usage: while (timer.ConsumeFixedStep()) { physicsWorld.Step(kFixedDt); }
    bool ConsumeFixedStep();

    double GetFixedDt() const { return m_fixedDt; }
    void   SetFixedDt(double dt) { m_fixedDt = dt; }

private:
    int64_t  m_frequency = 0;
    int64_t  m_lastTime = 0;
    int64_t  m_startTime = 0;
    double   m_totalTime = 0.0;
    uint64_t m_frameCount = 0;

    // FPS smoothing
    double   m_fpsAccum = 0.0;
    int      m_fpsFrames = 0;
    float    m_fps = 0.0f;

    // Fixed timestep
    double   m_fixedDt = 1.0 / 60.0;      // 60 Hz physics by default
    double   m_accumulator = 0.0;
    static constexpr double kMaxDt = 0.25; // Clamp frame dt to prevent spiral of death
};

} // namespace core
