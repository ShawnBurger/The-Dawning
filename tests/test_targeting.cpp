// =============================================================================
// tests/test_targeting.cpp — gameplay::targeting selection + firing-solution math
// =============================================================================
#include "test_framework.h"
#include "gameplay/targeting.h"

#include <cmath>
#include <vector>

using namespace gameplay;

namespace
{
TargetCandidate MakeBody(uint64_t id, const char* name,
                         core::Vec3d pos, core::Vec3d vel, double radius)
{
    TargetCandidate c;
    c.id = id; c.name = name; c.worldPos = pos; c.worldVel = vel; c.radius = radius;
    return c;
}
} // namespace

// --- ComputeTargetInfo -------------------------------------------------------
TEST_CASE(Targeting_ClosingSpeedPositiveWhenApproaching)
{
    // Ship at origin at rest; target 1000 m out on +x moving toward it at 100 m/s.
    TargetCandidate t = MakeBody(10, "T", { 1000, 0, 0 }, { -100, 0, 0 }, 50);
    TargetInfo info = ComputeTargetInfo({ 0, 0, 0 }, { 0, 0, 0 }, t);
    CHECK(info.valid);
    CHECK_APPROX(info.rangeMeters, 1000.0);
    CHECK_APPROX(info.relativeSpeed, 100.0);
    CHECK_APPROX(info.closingSpeed, 100.0);   // range shrinking -> positive
}

TEST_CASE(Targeting_ClosingSpeedNegativeWhenOpening)
{
    TargetCandidate t = MakeBody(10, "T", { 1000, 0, 0 }, { 250, 0, 0 }, 50);
    TargetInfo info = ComputeTargetInfo({ 0, 0, 0 }, { 0, 0, 0 }, t);
    CHECK_APPROX(info.closingSpeed, -250.0);  // range growing -> negative
}

TEST_CASE(Targeting_ClosingUsesRelativeVelocity)
{
    // Ship and target moving together at the same velocity -> zero closing, though
    // each has a large absolute speed.
    TargetCandidate t = MakeBody(10, "T", { 1000, 0, 0 }, { 0, 5000, 0 }, 50);
    TargetInfo info = ComputeTargetInfo({ 0, 0, 0 }, { 0, 5000, 0 }, t);
    CHECK_APPROX(info.closingSpeed, 0.0);
    CHECK_APPROX(info.relativeSpeed, 0.0);
}

// --- ComputeFiringSolution ---------------------------------------------------
TEST_CASE(Targeting_FiringSolutionStationaryAimsStraight)
{
    // Stationary target: aim point == target, time = range / muzzle speed.
    FiringSolution s = ComputeFiringSolution({ 0, 0, 0 }, { 0, 0, 0 },
                                             { 1000, 0, 0 }, { 0, 0, 0 }, 500.0);
    CHECK(s.valid);
    CHECK_APPROX(s.timeToImpact, 2.0);
    CHECK_APPROX(s.worldPos.x, 1000.0);
    CHECK_APPROX(s.worldPos.y, 0.0);
}

TEST_CASE(Targeting_FiringSolutionLeadsMovingTarget)
{
    // Target crossing at 100 m/s in +y; the lead point is ahead in +y, and the
    // intercept satisfies |aim - shooter| == muzzle*t.
    const double w = 500.0;
    FiringSolution s = ComputeFiringSolution({ 0, 0, 0 }, { 0, 0, 0 },
                                             { 1000, 0, 0 }, { 0, 100, 0 }, w);
    CHECK(s.valid);
    CHECK(s.worldPos.y > 100.0); // led ahead of the current position
    const double dx = s.worldPos.x, dy = s.worldPos.y, dz = s.worldPos.z;
    const double reach = std::sqrt(dx * dx + dy * dy + dz * dz);
    CHECK_APPROX_EPS(reach, w * s.timeToImpact, 1e-3); // projectile reaches the pip
}

TEST_CASE(Targeting_FiringSolutionInheritsShipVelocity)
{
    // Ship and target share velocity -> V_rel == 0 -> aim straight at the target
    // (the round inherits the ship velocity), same as the stationary case.
    FiringSolution s = ComputeFiringSolution({ 0, 0, 0 }, { 0, 200, 0 },
                                             { 1000, 0, 0 }, { 0, 200, 0 }, 500.0);
    CHECK(s.valid);
    CHECK_APPROX(s.worldPos.x, 1000.0);
    CHECK_APPROX(s.worldPos.y, 0.0);
}

TEST_CASE(Targeting_FiringSolutionNoneWhenTargetOutruns)
{
    // Target opening directly away at 600 m/s with a 500 m/s round: no intercept.
    FiringSolution s = ComputeFiringSolution({ 0, 0, 0 }, { 0, 0, 0 },
                                             { 1000, 0, 0 }, { 600, 0, 0 }, 500.0);
    CHECK_FALSE(s.valid);
}

TEST_CASE(Targeting_FiringSolutionRejectsNonPositiveSpeed)
{
    FiringSolution s = ComputeFiringSolution({ 0, 0, 0 }, { 0, 0, 0 },
                                             { 1000, 0, 0 }, { 0, 0, 0 }, 0.0);
    CHECK_FALSE(s.valid);
}

// --- Selection ---------------------------------------------------------------
TEST_CASE(Targeting_SelectNearestPicksClosest)
{
    std::vector<TargetCandidate> cs = {
        MakeBody(1, "A", { 5000, 0, 0 }, {}, 10),
        MakeBody(2, "B", { 800, 0, 0 },  {}, 10),
        MakeBody(3, "C", { 3000, 0, 0 }, {}, 10),
    };
    CHECK_EQ(SelectNearest(cs, { 0, 0, 0 }), 2ull);
    // Excluding the nearest returns the next nearest.
    CHECK_EQ(SelectNearest(cs, { 0, 0, 0 }, 2), 3ull);
    CHECK_EQ(SelectNearest({}, { 0, 0, 0 }), 0ull);
}

TEST_CASE(Targeting_CycleIsDeterministicAndWraps)
{
    // Insertion order deliberately not ascending; the cycle is by ascending id.
    std::vector<TargetCandidate> cs = {
        MakeBody(30, "C", { 0, 0, 0 }, {}, 10),
        MakeBody(10, "A", { 0, 0, 0 }, {}, 10),
        MakeBody(20, "B", { 0, 0, 0 }, {}, 10),
    };
    CHECK_EQ(CycleTarget(cs, 0, +1), 10ull);   // no selection -> first
    CHECK_EQ(CycleTarget(cs, 10, +1), 20ull);
    CHECK_EQ(CycleTarget(cs, 20, +1), 30ull);
    CHECK_EQ(CycleTarget(cs, 30, +1), 10ull);  // wrap forward
    CHECK_EQ(CycleTarget(cs, 10, -1), 30ull);  // wrap backward
    CHECK_EQ(CycleTarget(cs, 999, +1), 10ull); // stale id -> first
    CHECK_EQ(CycleTarget({}, 10, +1), 0ull);
}

TEST_CASE(Targeting_FindCandidate)
{
    std::vector<TargetCandidate> cs = {
        MakeBody(10, "A", { 0, 0, 0 }, {}, 10),
        MakeBody(20, "B", { 0, 0, 0 }, {}, 10),
    };
    CHECK_EQ(FindCandidate(cs, 20), 1);
    CHECK_EQ(FindCandidate(cs, 99), -1);
}
