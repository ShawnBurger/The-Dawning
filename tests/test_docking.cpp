// =============================================================================
// tests/test_docking.cpp — gameplay::docking guidance + state machine
// =============================================================================
#include "test_framework.h"
#include "gameplay/docking.h"

#include <cmath>

using namespace gameplay;

namespace
{
// Canonical port at the origin, approach axis +z, up +y.
DockingPort MakePort()
{
    DockingPort p;
    p.position     = { 0, 0, 0 };
    p.approachAxis = { 0, 0, 1 };
    p.up           = { 0, 1, 0 };
    return p;
}
} // namespace

// --- Governor ----------------------------------------------------------------
TEST_CASE(Docking_GovernorBrakesAndClamps)
{
    DockingPort p = MakePort();
    // At/inside the capture point the limit is the terminal creep vMin.
    CHECK_APPROX(MaxApproachSpeed(p.captureRadius, p), p.vMin);
    CHECK_APPROX(MaxApproachSpeed(0.0, p), p.vMin);
    // Far out it saturates at vCruise.
    CHECK_APPROX(MaxApproachSpeed(100000.0, p), p.vCruise);
    // Monotonic non-decreasing with distance in the braking region.
    CHECK(MaxApproachSpeed(50.0, p) >= MaxApproachSpeed(20.0, p));
    CHECK(MaxApproachSpeed(20.0, p) >= MaxApproachSpeed(p.captureRadius, p));
}

// --- Approach geometry -------------------------------------------------------
TEST_CASE(Docking_OnAxisNosedInHasZeroErrors)
{
    DockingPort p = MakePort();
    // 100 m out along +z, nose pointing back at the port (-z), up matched, closing.
    DockApproach a = ComputeApproach({ 0, 0, 100 }, { 0, 0, -10 },
                                     { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK_APPROX(a.along, 100.0);
    CHECK_APPROX_EPS(a.lateral, 0.0, 1e-6);
    CHECK_APPROX_EPS(a.coneError, 0.0, 1e-6);
    CHECK_APPROX_EPS(a.alignError, 0.0, 1e-6);
    CHECK_APPROX_EPS(a.rollError, 0.0, 1e-6);
    CHECK_APPROX(a.closingSpeed, 10.0);   // range shrinking
    CHECK(a.inCorridor);
}

TEST_CASE(Docking_OpeningClosingSpeedIsNegative)
{
    DockingPort p = MakePort();
    DockApproach a = ComputeApproach({ 0, 0, 100 }, { 0, 0, 10 },
                                     { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK_APPROX(a.closingSpeed, -10.0);
}

TEST_CASE(Docking_LateralOffsetLeavesCorridor)
{
    DockingPort p = MakePort();
    // Way off to the side and only slightly out along the axis.
    DockApproach a = ComputeApproach({ 80, 0, 10 }, { 0, 0, 0 },
                                     { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK(a.lateral > 70.0);
    CHECK(a.coneError > p.coneHalfAngle);
    CHECK_FALSE(a.inCorridor);
    CHECK_FALSE(a.captured);
}

TEST_CASE(Docking_CapturesWhenCloseAlignedAndSlow)
{
    DockingPort p = MakePort();
    DockApproach a = ComputeApproach({ 0, 0, 5 }, { 0, 0, -1 },
                                     { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK(a.captured);
}

TEST_CASE(Docking_NoCaptureWhenMisaligned)
{
    DockingPort p = MakePort();
    // Close and slow, but nosed sideways (not into the port).
    DockApproach a = ComputeApproach({ 0, 0, 5 }, { 0, 0, -1 },
                                     { 1, 0, 0 }, { 0, 1, 0 }, p);
    CHECK(a.alignError > p.alignTol);
    CHECK_FALSE(a.captured);
}

TEST_CASE(Docking_NoCaptureWhenTooFast)
{
    DockingPort p = MakePort();
    // Close and aligned but coming in far too hot.
    DockApproach a = ComputeApproach({ 0, 0, 5 }, { 0, 0, -40 },
                                     { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK_FALSE(a.captured);
    CHECK(a.overspeed);
}

// --- State machine -----------------------------------------------------------
TEST_CASE(Docking_IdleEngagesInCorridorInRange)
{
    DockingPort p = MakePort();
    DockApproach a = ComputeApproach({ 0, 0, 150 }, { 0, 0, -5 },
                                     { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK_EQ(static_cast<int>(StepDockState(DockState::Idle, a, p, 200.0)),
             static_cast<int>(DockState::Approaching));
    // Out of range stays idle.
    DockApproach far = ComputeApproach({ 0, 0, 500 }, { 0, 0, -5 },
                                       { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK_EQ(static_cast<int>(StepDockState(DockState::Idle, far, p, 200.0)),
             static_cast<int>(DockState::Idle));
}

TEST_CASE(Docking_OverspeedForcesHoldAndRecovers)
{
    DockingPort p = MakePort();
    DockApproach hot = ComputeApproach({ 0, 0, 100 }, { 0, 0, -40 },
                                       { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK(hot.overspeed);
    CHECK_EQ(static_cast<int>(StepDockState(DockState::Approaching, hot, p, 200.0)),
             static_cast<int>(DockState::Hold));
    // Once slowed back inside the governor, Hold returns to Approaching.
    DockApproach ok = ComputeApproach({ 0, 0, 100 }, { 0, 0, -5 },
                                      { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK_FALSE(ok.overspeed);
    CHECK_EQ(static_cast<int>(StepDockState(DockState::Hold, ok, p, 200.0)),
             static_cast<int>(DockState::Approaching));
}

TEST_CASE(Docking_AligningCapturesToDocked)
{
    DockingPort p = MakePort();
    DockApproach cap = ComputeApproach({ 0, 0, 5 }, { 0, 0, -1 },
                                       { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK(cap.captured);
    CHECK_EQ(static_cast<int>(StepDockState(DockState::Aligning, cap, p, 200.0)),
             static_cast<int>(DockState::Docked));
    // Docked is sticky.
    CHECK_EQ(static_cast<int>(StepDockState(DockState::Docked, cap, p, 200.0)),
             static_cast<int>(DockState::Docked));
}

TEST_CASE(Docking_LeavingRangeReturnsToIdle)
{
    DockingPort p = MakePort();
    DockApproach far = ComputeApproach({ 0, 0, 400 }, { 0, 0, 0 },
                                       { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK_EQ(static_cast<int>(StepDockState(DockState::Approaching, far, p, 200.0)),
             static_cast<int>(DockState::Idle));
}

TEST_CASE(Docking_UndockingClearsWhenOutsideTheCollar)
{
    DockingPort p = MakePort();
    // Still inside the fine-approach envelope (captureRadius*4 = 48 m): stay Undocking.
    DockApproach nearPort = ComputeApproach({ 0, 0, 20 }, { 0, 0, 0 },
                                            { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK_EQ(static_cast<int>(StepDockState(DockState::Undocking, nearPort, p, 200.0)),
             static_cast<int>(DockState::Undocking));
    // Pushed clear along the axis -> free flight.
    DockApproach clear = ComputeApproach({ 0, 0, 60 }, { 0, 0, 0 },
                                         { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK_EQ(static_cast<int>(StepDockState(DockState::Undocking, clear, p, 200.0)),
             static_cast<int>(DockState::Idle));
}

TEST_CASE(Docking_UndockingIsNotStrandedByAnOffAxisPush)
{
    DockingPort p = MakePort();
    // Pushed sideways: along ~ 0 but the ship is well clear of the port (range 60 m).
    // Undocking must clear on RANGE, not `along` — an along-based test would strand the
    // ship here (along 0 < engageRange), unable to re-dock. This pins the fix.
    DockApproach sideways = ComputeApproach({ 60, 0, 0 }, { 0, 0, 0 },
                                            { 0, 0, -1 }, { 0, 1, 0 }, p);
    CHECK(sideways.along < 1.0);   // essentially no axial progress
    CHECK(sideways.range > 48.0);  // but genuinely clear of the collar
    CHECK_EQ(static_cast<int>(StepDockState(DockState::Undocking, sideways, p, 200.0)),
             static_cast<int>(DockState::Idle));
}
