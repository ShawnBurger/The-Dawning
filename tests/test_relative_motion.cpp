// =============================================================================
// tests/test_relative_motion.cpp — Clohessy-Wiltshire / Hill motion (Stage 10).
//
// Invariants:
//   * MeanMotion against a known circular-orbit value; Φ(0) = I.
//   * CW TARGETING CLOSURE — the v0 SolveCWTargeting returns, fed back through
//     PropagateCW, arrives at rf with the stated arrival velocity (self-checking).
//   * DRIFT-FREE bounded orbit — with vy0 = −2n·x0 the secular along-track drift
//     cancels, so the full state returns after one period (pins the secular STM
//     terms).
//   * NONLINEAR CROSS-CHECK — for a small separation, CW must match the relative
//     motion of two FULL two-body orbits (PropagateUniversal) expressed in the
//     target's rotating LVLH frame. This is what pins the FRAME CONVENTION
//     (radial-outward x, the Coriolis sign) against real physics; the other tests
//     would pass a self-consistent-but-wrong-framed STM.
// =============================================================================

#include "test_framework.h"
#include "sim/relative_motion.h"
#include "sim/kepler.h"       // StateVector, PropagateUniversal

#include <cmath>
#include <limits>

namespace
{
using core::Vec3d;
using namespace sim;

constexpr double kMu  = 3.986004418e14; // Earth
constexpr double kTwoPi = 6.28318530717958647692;

double AbsErr(const Vec3d& a, const Vec3d& b) { return (a - b).Length(); }

} // namespace

TEST_CASE(CW_MeanMotionAndIdentityAtZero)
{
    const double a = 7.0e6;
    const double n = MeanMotion(kMu, a);
    // n = sqrt(mu/a^3); period ~ 5828 s for a 7000 km orbit.
    CHECK_APPROX_EPS(n, std::sqrt(kMu / (a * a * a)), 1.0e-15);
    CHECK_APPROX_EPS(kTwoPi / n, 5828.5, 5.0);
    CHECK_EQ(MeanMotion(0.0, a), 0.0);
    CHECK_EQ(MeanMotion(kMu, -1.0), 0.0);

    // Φ(0) = I: propagating by zero time returns the input exactly.
    const CWState rel{ { 30.0, -40.0, 20.0 }, { 0.05, 0.03, -0.02 } };
    const CWState same = PropagateCW(rel, n, 0.0);
    CHECK_EQ(AbsErr(same.position, rel.position), 0.0);
    CHECK_EQ(AbsErr(same.velocity, rel.velocity), 0.0);
}

TEST_CASE(CW_TargetingClosure)
{
    const double n = MeanMotion(kMu, 7.0e6);
    const Vec3d r0{ 100.0, -50.0, 30.0 };
    const Vec3d rf{ -20.0, 80.0, -10.0 };
    const double t = 1500.0; // a general time, well away from k·period

    const CWTargeting plan = SolveCWTargeting(r0, rf, n, t);
    CHECK(plan.feasible);
    if (!plan.feasible) return;

    // The solved v0 must fly r0 -> rf, arriving with the stated velocity.
    const CWState flown = PropagateCW(CWState{ r0, plan.v0 }, n, t);
    CHECK(AbsErr(flown.position, rf) < 1.0e-6);
    CHECK(AbsErr(flown.velocity, plan.arrivalVel) < 1.0e-9);

    // Singular geometry (t = one period) is rejected, not silently wrong.
    CHECK_FALSE(SolveCWTargeting(r0, rf, n, kTwoPi / n).feasible);
}

TEST_CASE(CW_TargetingRejectsSingularGeometries)
{
    const double n = MeanMotion(kMu, 7.0e6);
    const Vec3d r0{ 100.0, -50.0, 30.0 };
    const Vec3d rf{ -20.0, 80.0, -10.0 };

    // (1) INTERIOR in-plane singularity: det·n² = 8(1−cos u) − 3u·sin u has a root
    // near u ≈ 8.84 (≈ 1.4 periods), NOT at an integer period. Bisect for it; the
    // targeting there is genuinely singular and must be rejected. (The old
    // |det|<1e-300 guard admitted this with |v0| ~ 1e12 m/s.)
    auto detN2 = [](double u) {
        return 8.0 * (1.0 - std::cos(u)) - 3.0 * u * std::sin(u);
    };
    double lo = 8.7, hi = 8.9; // detN2(8.7) < 0 < detN2(8.9)
    for (int i = 0; i < 200; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        if (detN2(lo) * detN2(mid) <= 0.0) hi = mid; else lo = mid;
    }
    const double uRoot = 0.5 * (lo + hi);
    CHECK(uRoot > 8.7 && uRoot < 8.9); // an interior root, not a multiple of period
    CHECK_FALSE(SolveCWTargeting(r0, rf, n, uRoot / n).feasible);

    // (2) CROSS-TRACK near-singularity: nt just past π gives |sin nt| ~ 1e-9, which
    // the old |s|<1e-12 guard admitted (|v0z| ~ 1e9·Δz). Must be rejected.
    const double tCross = (3.14159265358979323846 + 1.0e-9) / n;
    CHECK_FALSE(SolveCWTargeting(r0, rf, n, tCross).feasible);

    // A well-conditioned time between the singularities still succeeds (the guard
    // does not over-reject).
    CHECK(SolveCWTargeting(r0, rf, n, 1500.0).feasible);
}

TEST_CASE(CW_DriftFreeOrbitClosesAfterOnePeriod)
{
    const double n = MeanMotion(kMu, 7.0e6);
    const double period = kTwoPi / n;

    // vy0 = −2·n·x0 kills the secular along-track drift; the relative orbit is
    // then a closed 2:1 ellipse and the FULL state returns after one period,
    // independent of the other components.
    const double x0 = 60.0;
    const CWState rel{ { x0, 25.0, 15.0 },
                       { 0.1, -2.0 * n * x0, 0.07 } };
    const CWState after = PropagateCW(rel, n, period);
    CHECK(AbsErr(after.position, rel.position) < 1.0e-6);
    CHECK(AbsErr(after.velocity, rel.velocity) < 1.0e-9);

    // Negative control: WITHOUT the drift-free condition the along-track position
    // drifts by a full secular term over one period (this is not a bug — it is the
    // physics the condition above cancels, asserted so the closure above is not
    // vacuous).
    const CWState drifting{ { x0, 25.0, 0.0 }, { 0.0, 0.0, 0.0 } };
    const CWState drifted = PropagateCW(drifting, n, period);
    CHECK(std::fabs(drifted.position.y - drifting.position.y) > 1000.0);
}

TEST_CASE(CW_MatchesFullTwoBodyRelativeMotionForSmallSeparation)
{
    const double a = 7.0e6;
    const double n = MeanMotion(kMu, a);
    const double vCirc = std::sqrt(kMu / a);

    // Target on a circular, planar (xy) prograde orbit. At t=0 its LVLH axes are
    // the inertial x (radial), y (along-track), z (cross-track).
    const StateVector target0{ { a, 0.0, 0.0 }, { 0.0, vCirc, 0.0 } };

    // Small relative state in the LVLH (rotating) frame.
    const Vec3d r0{ 30.0, -40.0, 20.0 };
    const Vec3d v0rot{ 0.05, 0.03, -0.02 };

    // Embed the chaser in inertial coordinates at t=0 (R = I). The inertial
    // relative velocity carries the frame-rotation term: v_inertial = v_rot + ω×r,
    // ω = n·ẑ, so ω×r0 = n(−r0.y, r0.x, 0).
    const Vec3d chaserPos0 = target0.position + r0;
    const Vec3d chaserVel0 =
        target0.velocity + v0rot + Vec3d{ -n * r0.y, n * r0.x, 0.0 };

    const double t = 0.15 * (kTwoPi / n); // a fraction of an orbit

    bool okT = false, okC = false;
    const StateVector targetT =
        PropagateUniversal(target0, kMu, t, &okT);
    const StateVector chaserT =
        PropagateUniversal(StateVector{ chaserPos0, chaserVel0 }, kMu, t, &okC);
    CHECK(okT);
    CHECK(okC);

    // Build the target's LVLH frame at time t and project the relative state.
    const Vec3d rHat = targetT.position / targetT.position.Length();
    const Vec3d yHat = targetT.velocity / targetT.velocity.Length(); // circular ⇒ ⊥ rHat
    const Vec3d zHat = rHat.Cross(yHat);

    const Vec3d relPosI = chaserT.position - targetT.position;
    const Vec3d relVelI = chaserT.velocity - targetT.velocity;
    const Vec3d rLvlh{ relPosI.Dot(rHat), relPosI.Dot(yHat), relPosI.Dot(zHat) };
    const Vec3d vInFrame{ relVelI.Dot(rHat), relVelI.Dot(yHat), relVelI.Dot(zHat) };
    // Rotating-frame velocity = frame-projected inertial velocity − ω×r_LVLH.
    const Vec3d vLvlh = vInFrame - Vec3d{ -n * rLvlh.y, n * rLvlh.x, 0.0 };

    // CW's prediction of the same relative state.
    const CWState cw = PropagateCW(CWState{ r0, v0rot }, n, t);

    // First-order agreement: the linearisation error scales with separation²/a
    // (~sub-metre here), so CW and the full two-body relative motion agree tightly.
    CHECK(AbsErr(cw.position, rLvlh) < 0.5);   // metres
    CHECK(AbsErr(cw.velocity, vLvlh) < 1.0e-3); // m/s
}
