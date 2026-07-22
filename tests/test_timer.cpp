// =============================================================================
// tests/test_timer.cpp - fixed-step timer contract
// =============================================================================

#include "test_framework.h"
#include "core/timer.h"

#include <limits>

TEST_CASE(Timer_FixedStepRejectsValuesThatCouldHangTheDrainLoop)
{
    core::Timer timer;
    const double original = timer.GetFixedDt();

    CHECK_FALSE(timer.SetFixedDt(0.0));
    CHECK_FALSE(timer.SetFixedDt(-0.1));
    CHECK_FALSE(timer.SetFixedDt((std::numeric_limits<double>::infinity)()));
    CHECK_FALSE(timer.SetFixedDt((std::numeric_limits<double>::quiet_NaN)()));
    CHECK_EQ(timer.GetFixedDt(), original);

    CHECK(timer.SetFixedDt(1.0 / 120.0));
    CHECK_EQ(timer.GetFixedDt(), 1.0 / 120.0);
}

TEST_CASE(Timer_DiscardOnEmptyAccumulatorIsInert)
{
    core::Timer timer;
    timer.DiscardFixedSteps();
    CHECK_FALSE(timer.ConsumeFixedStep());
}

TEST_CASE(Timer_TimeAccelerationRunsMoreFixedStepsButStaysBounded)
{
    core::Timer timer;
    CHECK(timer.SetFixedDt(0.01)); // 100 Hz for clean arithmetic
    auto drain = [&]() { int n = 0; while (timer.ConsumeFixedStep()) ++n; return n; };

    const double frame = 0.05; // one "frame" of wall time

    // Real time (scale 1): 0.05 / 0.01 ≈ 5 fixed steps. Behaviour is unchanged.
    CHECK_EQ(timer.GetTimeScale(), 1.0);
    timer.AdvanceTime(frame);
    const int base = drain();
    CHECK(base >= 4 && base <= 5);

    // 10x acceleration: an order of magnitude MORE steps, deterministically.
    CHECK(timer.SetTimeScale(10.0));
    CHECK_EQ(timer.GetTimeScale(), 10.0);
    timer.AdvanceTime(frame);
    const int fast = drain();
    CHECK(fast >= 49 && fast <= 51); // ~50, and clearly 10x the base

    // Spiral-of-death cap: an absurd scale is BOUNDED, never an unbounded step
    // count that would freeze the loop (kMaxStepsPerTick worth of work).
    CHECK(timer.SetTimeScale(1.0e12));
    timer.AdvanceTime(frame);
    const int capped = drain();
    CHECK(capped >= 599 && capped <= 600);

    // Invalid scales are rejected and the previous scale is kept.
    CHECK_FALSE(timer.SetTimeScale(0.0));
    CHECK_FALSE(timer.SetTimeScale(-2.0));
    CHECK_FALSE(timer.SetTimeScale((std::numeric_limits<double>::infinity)()));
    CHECK_EQ(timer.GetTimeScale(), 1.0e12);
}
