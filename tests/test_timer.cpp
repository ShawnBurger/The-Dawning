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
