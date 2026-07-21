// =============================================================================
// tests/test_lambert.cpp — universal-variable Lambert solver (Sim Stage 8).
//
// Load-bearing invariant: a Lambert solution must actually FLY the transfer.
//   (a) LAMBERT-THEN-PROPAGATE closure — PropagateUniversal(r1, v1, tof) lands on
//       r2 and reaches v2. Validates Lambert against the shipped propagator for
//       an arbitrary geometry with NO external reference numbers.
//   (b) KNOWN-ORBIT round-trip — sample (r1,v1) and (r2,v2) from a real orbit,
//       hand Lambert (r1,r2,tof), and recover the orbit's own v1,v2 (the zero-rev
//       solution in a fixed direction is unique, so it must be the source orbit).
// Both are exercised across a short arc (<180°), a long arc (>180°), and a
// hyperbolic transfer (z<0), plus degenerate-geometry rejection.
// =============================================================================

#include "test_framework.h"
#include "sim/lambert.h"
#include "sim/kepler.h"        // StateVector, ElementsToState, PropagateUniversal
#include "ecs/components.h"    // OrbitalElements

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

// (a) Lambert-then-propagate: v1 from Lambert must fly r1 -> r2 in tof, arriving
// at v2. Self-checking against PropagateUniversal; no reference velocities.
void CheckClosure(const Vec3d& r1, const Vec3d& r2, double tof, double mu,
                  bool prograde)
{
    const LambertSolution sol = SolveLambert(r1, r2, tof, mu, prograde);
    CHECK(sol.converged);
    if (!sol.converged) return;

    bool ok = false;
    const StateVector flown = PropagateUniversal(
        StateVector{ r1, sol.v1 }, mu, tof, &ok);
    CHECK(ok);
    CHECK(Rel(flown.position, r2) < 1.0e-8);   // arrives at r2
    CHECK(Rel(flown.velocity, sol.v2) < 1.0e-8); // with Lambert's stated v2
}

} // namespace

TEST_CASE(Lambert_ClosureShortAndLongWayAndOffPlane)
{
    const double mu = 3.986004418e14; // Earth, m^3/s^2

    // A published geometry (Curtis Ex. 5.2, in SI): validates a real short-way
    // prograde transfer end to end without asserting its reference velocities.
    CheckClosure(Vec3d{ 5.0e6, 1.0e7, 2.1e6 },
                 Vec3d{ -1.46e7, 2.5e6, 7.0e6 }, 3600.0, mu, /*prograde=*/true);

    // Same endpoints, the OTHER sense: retrograde is a different, still-valid
    // transfer that must also close its own loop.
    CheckClosure(Vec3d{ 5.0e6, 1.0e7, 2.1e6 },
                 Vec3d{ -1.46e7, 2.5e6, 7.0e6 }, 3600.0, mu, /*prograde=*/false);

    // Planar transfers that run BOTH transfer-angle branches. The swept angle is
    // fixed by geometry + sense, NOT by tof: (a×b).z > 0 is the short way
    // (Δν = 90°); endpoints with (a×c).z < 0 force the prograde LONG way
    // (Δν = 270°, the lambert.cpp:46 flip). CheckClosure only proves each produces
    // a valid transfer — WHICH sense the solver selects is discriminated by the
    // recovery tests below, because both senses close this loop.
    const Vec3d a{ 1.2e7, 0.0, 0.0 };
    const Vec3d bShort{ 0.0, 1.1e7, 0.0 };  // (a×bShort).z > 0 -> 90°  (short way)
    const Vec3d cLong{ 0.0, -1.1e7, 0.0 };  // (a×cLong).z  < 0 -> 270° (long way)
    CheckClosure(a, bShort, 1500.0, mu, true);
    CheckClosure(a, cLong, 9000.0, mu, true);
}

TEST_CASE(Lambert_RecoversAKnownEllipticOrbitVelocities)
{
    const double mu = 3.986004418e14;
    ecs::OrbitalElements el;
    el.semiMajorAxis   = 1.0e7;
    el.eccentricity    = 0.3;
    el.inclination     = 0.5;   // < 90° => prograde (h_z > 0)
    el.longitudeAscNode = 0.4;
    el.argPeriapsis    = 0.6;
    el.trueAnomaly     = 0.2;

    const double period = 2.0 * 3.14159265358979323846 *
                          std::sqrt(el.semiMajorAxis * el.semiMajorAxis *
                                    el.semiMajorAxis / mu);

    const StateVector s1 = ElementsToState(el, mu);

    // Short arc (0.3 period) and long arc (0.7 period) — both are one-rev
    // prograde transfers, so Lambert must recover the orbit's OWN v1, v2.
    for (double frac : { 0.3, 0.7 })
    {
        const double tof = frac * period;
        bool ok = false;
        const StateVector s2 = PropagateUniversal(s1, mu, tof, &ok);
        CHECK(ok);

        const LambertSolution sol =
            SolveLambert(s1.position, s2.position, tof, mu, /*prograde=*/true);
        CHECK(sol.converged);
        if (!sol.converged) continue;
        CHECK(Rel(sol.v1, s1.velocity) < 1.0e-7);
        CHECK(Rel(sol.v2, s2.velocity) < 1.0e-7);
    }
}

TEST_CASE(Lambert_RecoversRetrogradeOrbitAndSensesDiffer)
{
    // A RETROGRADE orbit (inclination > 90°, so h_z < 0) must be recovered with
    // prograde=false. CheckClosure cannot discriminate the sense — BOTH the
    // prograde and retrograde solutions are genuine Kepler arcs from r1 to r2 in
    // tof, so both close the loop — which is why the retrograde branch
    // (lambert.cpp:47) needs a velocity-RECOVERY witness and a senses-differ check.
    const double mu = 3.986004418e14;
    ecs::OrbitalElements el;
    el.semiMajorAxis    = 1.1e7;
    el.eccentricity     = 0.25;
    el.inclination      = 2.5;   // ~143° => retrograde (h_z < 0)
    el.longitudeAscNode = 0.7;
    el.argPeriapsis     = 0.3;
    el.trueAnomaly      = 0.15;

    const double period = 2.0 * 3.14159265358979323846 *
                          std::sqrt(el.semiMajorAxis * el.semiMajorAxis *
                                    el.semiMajorAxis / mu);
    const StateVector s1 = ElementsToState(el, mu);

    // The 0.7-period arc sweeps > 180° in-plane -> (r1×r2).z > 0 for a retrograde
    // orbit, so the line-47 flip is load-bearing; breaking it fails recovery here.
    for (double frac : { 0.3, 0.7 })
    {
        const double tof = frac * period;
        bool ok = false;
        const StateVector s2 = PropagateUniversal(s1, mu, tof, &ok);
        CHECK(ok);
        const LambertSolution sol =
            SolveLambert(s1.position, s2.position, tof, mu, /*prograde=*/false);
        CHECK(sol.converged);
        if (!sol.converged) continue;
        CHECK(Rel(sol.v1, s1.velocity) < 1.0e-7); // recovers the RETROGRADE v1
        CHECK(Rel(sol.v2, s2.velocity) < 1.0e-7);
    }

    // The two senses are DIFFERENT transfers for identical endpoints + tof. Solving
    // prograde must NOT collapse onto the retrograde solution — the direct witness
    // that lambert.cpp:46/47 actually branch on `prograde`.
    bool ok = false;
    const StateVector s2 = PropagateUniversal(s1, mu, 0.5 * period, &ok);
    CHECK(ok);
    const LambertSolution pro =
        SolveLambert(s1.position, s2.position, 0.5 * period, mu, true);
    const LambertSolution ret =
        SolveLambert(s1.position, s2.position, 0.5 * period, mu, false);
    CHECK(pro.converged);
    CHECK(ret.converged);
    if (pro.converged && ret.converged)
        CHECK((pro.v1 - ret.v1).Length() > 1.0); // meaningfully different (m/s)
}

TEST_CASE(Lambert_HyperbolicTransferClosesTheLoop)
{
    const double mu = 3.986004418e14;
    // A hyperbolic state: |v| just above escape at r (sqrt(2mu/r) ~ 8930 m/s here),
    // so the transfer conic has z < 0 in the universal-variable iteration.
    const Vec3d r1{ 1.0e7, 0.0, 0.0 };
    const Vec3d v1{ 2000.0, 9000.0, 500.0 }; // |v| ~ 9220 m/s > escape

    bool ok = false;
    const StateVector s2 = PropagateUniversal(StateVector{ r1, v1 }, mu, 1000.0, &ok);
    CHECK(ok);

    const LambertSolution sol =
        SolveLambert(r1, s2.position, 1000.0, mu, /*prograde=*/true);
    CHECK(sol.converged);
    if (sol.converged)
    {
        CHECK(Rel(sol.v1, v1) < 1.0e-7);            // recovers the hyperbolic v1
        CHECK(Rel(sol.v2, s2.velocity) < 1.0e-7);
    }
}

TEST_CASE(Lambert_RejectsDegenerateGeometryAndBadInput)
{
    const double mu = 3.986004418e14;
    const Vec3d r1{ 1.0e7, 0.0, 0.0 };

    // Colinear r1, r2 (Δν = 0): transfer plane undefined -> not converged.
    CHECK_FALSE(SolveLambert(r1, Vec3d{ 2.0e7, 0.0, 0.0 }, 3000.0, mu, true)
                    .converged);
    // Antiparallel (Δν = π): also degenerate.
    CHECK_FALSE(SolveLambert(r1, Vec3d{ -2.0e7, 0.0, 0.0 }, 3000.0, mu, true)
                    .converged);

    const Vec3d r2{ 0.0, 1.0e7, 0.0 };
    CHECK_FALSE(SolveLambert(r1, r2, 0.0, mu, true).converged);      // tof = 0
    CHECK_FALSE(SolveLambert(r1, r2, -100.0, mu, true).converged);   // tof < 0
    CHECK_FALSE(SolveLambert(r1, r2, 3000.0, 0.0, true).converged);  // mu = 0
    CHECK_FALSE(SolveLambert(r1, r2, 3000.0,
                             std::numeric_limits<double>::quiet_NaN(), true)
                    .converged);                                     // mu NaN
    CHECK_FALSE(SolveLambert(Vec3d{ 0.0, 0.0, 0.0 }, r2, 3000.0, mu, true)
                    .converged);                                     // |r1| = 0
}
