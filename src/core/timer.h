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
    bool   SetFixedDt(double dt);

    // Suspend simulation without retaining an arbitrarily large catch-up debt.
    void DiscardFixedSteps() { m_accumulator = 0.0; }

    // Time acceleration. Physics advances `scale`x wall time by running MORE fixed
    // steps per frame (each still GetFixedDt() — deterministic, no accuracy loss),
    // capped per frame so a large scale cannot demand unbounded steps and freeze
    // the loop. `scale` must be > 0 and finite; an invalid value is rejected and
    // the previous scale kept. 1.0 == real time.
    bool   SetTimeScale(double scale);
    double GetTimeScale() const { return m_timeScale; }

    // Feed the fixed-step accumulator with `realDt` seconds of WALL time, scaled by
    // the time-acceleration factor and capped at kMaxStepsPerTick steps' worth of
    // work. Tick() calls this with the measured frame dt; exposed for headless
    // drivers and testing (the QPC-free seam of the accumulator logic).
    void   AdvanceTime(double realDt);

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
    double   m_timeScale = 1.0;           // physics : wall-time ratio (>= real time)
    static constexpr double kMaxDt = 0.25; // Clamp frame dt to prevent spiral of death
    static constexpr int kMaxStepsPerTick = 600; // spiral-of-death cap under warp
};

} // namespace core
