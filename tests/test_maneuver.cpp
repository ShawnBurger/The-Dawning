// =============================================================================
// tests/test_maneuver.cpp — impulsive maneuver planning (Sim Stage 9).
//
// Invariants:
//   * HohmannDeltaV against the textbook LEO->GEO budget (~3.9 km/s).
//   * ZERO-COST self-rendezvous — planning an intercept of your OWN future
//     position (target == chaser) needs no burn, because your current velocity
//     already flies that transfer. This witnesses that the burns genuinely
//     VANISH for a self-transfer, so it catches any error that makes them
//     non-zero (e.g. ADDING rather than subtracting the current velocity). It
//     does NOT pin the subtraction DIRECTION (transferV1 − chaserVel vs the
//     reverse): since both endpoints coincide, |a−b| == |b−a| ~ 0 either way —
//     that direction is discriminated by the closure test below.
//   * Intercept CLOSURE — applying the departure burn and flying tof (through the
//     shipped PropagateUniversal) arrives at the intercept point, and the arrival
//     burn matches the target's velocity there. Self-checking, no reference numbers.
// =============================================================================

#include "test_framework.h"
#include "sim/maneuver.h"
#include "sim/kepler.h"       // StateVector, ElementsToState, PropagateUniversal
#include "ecs/components.h"   // OrbitalElements

#include <cmath>
#include <limits>

namespace
{
using core::Vec3d;
using namespace sim;

double Rel(const Vec3d& a, const Vec3d& b)
{
    const double d = (a - b).Length();
    const double s = (std::max)(a.Length(), b.Length());
    return s > 0.0 ? d / s : d;
}

constexpr double kMu = 3.986004418e14; // Earth, m^3/s^2

StateVector StateFrom(double a, double e, double i, double node, double argp,
                      double nu)
{
    ecs::OrbitalElements el;
    el.semiMajorAxis = a;
    el.eccentricity = e;
    el.inclination = i;
    el.longitudeAscNode = node;
    el.argPeriapsis = argp;
    el.trueAnomaly = nu;
    return ElementsToState(el, kMu);
}

} // namespace

TEST_CASE(Maneuver_HohmannMatchesTheKnownLeoToGeoBudget)
{
    // 300 km LEO -> GEO. Textbook: ~2.42 km/s + ~1.47 km/s ≈ 3.9 km/s total.
    const double rLeo = 6.678e6;
    const double rGeo = 4.2164e7;
    const HohmannTransfer h = HohmannDeltaV(rLeo, rGeo, kMu);
    CHECK_APPROX_EPS(h.departV, 2425.0, 30.0);
    CHECK_APPROX_EPS(h.arriveV, 1467.0, 30.0);
    CHECK_APPROX_EPS(h.totalV, 3892.0, 40.0);

    // Symmetric: lowering GEO->LEO costs the same total.
    const HohmannTransfer back = HohmannDeltaV(rGeo, rLeo, kMu);
    CHECK_APPROX_EPS(back.totalV, h.totalV, 1.0e-6);

    // Degenerate inputs -> zero.
    CHECK_EQ(HohmannDeltaV(0.0, rGeo, kMu).totalV, 0.0);
    CHECK_EQ(HohmannDeltaV(rLeo, -1.0, kMu).totalV, 0.0);
    CHECK_EQ(HohmannDeltaV(rLeo, rGeo, 0.0).totalV, 0.0);
}

TEST_CASE(Maneuver_SelfRendezvousCostsNothing)
{
    // A chaser planning to "intercept" its OWN future position needs no burn: its
    // current velocity already flies that exact transfer. Target == chaser.
    const StateVector s = StateFrom(1.0e7, 0.3, 0.5, 0.4, 0.6, 0.2); // prograde (i<90°)
    const double period =
        2.0 * 3.14159265358979323846 * std::sqrt(1.0e21 / kMu);
    const double tof = 0.3 * period; // short arc (<180°), unambiguous sense

    const InterceptPlan plan =
        PlanIntercept(s.position, s.velocity, s.position, s.velocity, tof, kMu,
                      /*prograde=*/true);
    CHECK(plan.feasible);
    // Both burns are ~zero (relative to the orbital speed ~ km/s scale).
    CHECK(plan.departureCost < 1.0e-3);
    CHECK(plan.arrivalCost < 1.0e-3);
    // The intercept point is the chaser's own future position.
    bool ok = false;
    const StateVector future = PropagateUniversal(s, kMu, tof, &ok);
    CHECK(ok);
    CHECK(Rel(plan.interceptPoint, future.position) < 1.0e-9);
}

TEST_CASE(Maneuver_InterceptPlanTrajectoryActuallyConnects)
{
    // Distinct chaser and target orbits. The plan must physically connect them.
    const StateVector chaser = StateFrom(1.0e7, 0.15, 0.4, 0.2, 0.5, 0.1);
    const StateVector target = StateFrom(1.4e7, 0.25, 0.6, 0.9, 0.3, 2.0);
    const double tof = 2600.0;

    const InterceptPlan plan =
        PlanIntercept(chaser.position, chaser.velocity,
                      target.position, target.velocity, tof, kMu, /*prograde=*/true);
    CHECK(plan.feasible);
    if (!plan.feasible) return;

    // The intercept point is the target's propagated position.
    bool ok = false;
    const StateVector tgtArrival = PropagateUniversal(target, kMu, tof, &ok);
    CHECK(ok);
    CHECK(Rel(plan.interceptPoint, tgtArrival.position) < 1.0e-9);

    // Apply the departure burn and FLY the transfer: it must arrive at the
    // intercept point, and adding the arrival burn must match the target's
    // velocity there (full rendezvous).
    const Vec3d transferV1 = chaser.velocity + plan.departureBurn;
    bool ok2 = false;
    const StateVector flown =
        PropagateUniversal(StateVector{ chaser.position, transferV1 }, kMu, tof, &ok2);
    CHECK(ok2);
    CHECK(Rel(flown.position, plan.interceptPoint) < 1.0e-7);
    const Vec3d matched = flown.velocity + plan.arrivalBurn;
    CHECK(Rel(matched, tgtArrival.velocity) < 1.0e-7);

    // The burns are nonzero for genuinely different orbits, and the total is their
    // sum (guards against a swapped/aliased cost field).
    CHECK(plan.departureCost > 1.0);
    CHECK_APPROX_EPS(plan.totalCost, plan.departureCost + plan.arrivalCost, 1.0e-9);
}

TEST_CASE(Maneuver_RejectsInfeasiblePlan)
{
    const StateVector chaser = StateFrom(1.0e7, 0.15, 0.4, 0.2, 0.5, 0.1);
    const StateVector target = StateFrom(1.4e7, 0.25, 0.6, 0.9, 0.3, 2.0);
    // tof <= 0 and non-finite mu propagate to feasible=false with zero burns.
    CHECK_FALSE(PlanIntercept(chaser.position, chaser.velocity,
                              target.position, target.velocity, 0.0, kMu, true)
                    .feasible);
    CHECK_FALSE(PlanIntercept(chaser.position, chaser.velocity,
                              target.position, target.velocity, 2600.0,
                              std::numeric_limits<double>::quiet_NaN(), true)
                    .feasible);

    // A non-finite CHASER VELOCITY is the one input that bypasses BOTH delegated
    // guards (SolveLambert guards chaserPos; PropagateUniversal guards the
    // target). Without an explicit check it would flow into departureBurn as a
    // NaN yet be reported feasible=true — so it must be rejected outright.
    const Vec3d nanVel{ std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0 };
    const InterceptPlan bad =
        PlanIntercept(chaser.position, nanVel, target.position, target.velocity,
                      2600.0, kMu, true);
    CHECK_FALSE(bad.feasible);
    CHECK_EQ(bad.departureCost, 0.0); // no NaN leaks into the returned plan
}
