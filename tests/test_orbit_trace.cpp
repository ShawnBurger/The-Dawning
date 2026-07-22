// =============================================================================
// tests/test_orbit_trace.cpp — orbit polyline sampling for the map/orrery view.
//
// Invariants:
//   * an ellipse samples to a CLOSED loop whose nearest point is periapsis
//     a(1-e) and farthest is apoapsis a(1+e), every point in between, and the
//     loop lies in the orbital plane (constant normal);
//   * the path reuses the shipped ElementsToState, so its periapsis/apoapsis
//     coincide with the propagated body's extremes;
//   * a hyperbola samples to an OPEN arc outside periapsis;
//   * degenerate elements sample to nothing.
// =============================================================================

#include "test_framework.h"
#include "sim/orbit_trace.h"
#include "sim/kepler.h"

#include <cmath>

namespace
{
using core::Vec3d;
using sim::SampleOrbitPath;

constexpr double kEarthMu = 3.986004418e14; // m^3/s^2

ecs::OrbitalElements MakeElements(double a, double e)
{
    ecs::OrbitalElements el;
    el.semiMajorAxis    = a;
    el.eccentricity     = e;
    el.inclination      = 0.4;
    el.longitudeAscNode = 1.0;
    el.argPeriapsis     = 0.6;
    el.trueAnomaly      = 2.3; // deliberately off periapsis: the path must not depend on it
    return el;
}
} // namespace

TEST_CASE(OrbitTrace_EllipseIsClosedLoopBetweenPeriapsisAndApoapsis)
{
    const double a = 2.0e7, e = 0.5;
    const uint32_t segs = 128;
    const std::vector<Vec3d> path = SampleOrbitPath(MakeElements(a, e), kEarthMu, segs);

    CHECK_EQ(path.size(), static_cast<size_t>(segs) + 1u);
    if (path.empty()) return;

    // Closed: last point coincides with the first.
    CHECK((path.back() - path.front()).Length() < 1.0); // < 1 m over a ~2e7 m orbit

    const double rp = a * (1.0 - e); // periapsis 1.0e7
    const double ra = a * (1.0 + e); // apoapsis  3.0e7
    double rmin = 1e300, rmax = 0.0;
    for (const Vec3d& p : path)
    {
        const double r = p.Length();
        rmin = (std::min)(rmin, r);
        rmax = (std::max)(rmax, r);
        // Every sample sits on the orbit, so its radius is within [rp, ra].
        CHECK(r > rp - 1.0 && r < ra + 1.0);
    }
    // The extremes are actually reached (E=0 -> periapsis, E=pi -> apoapsis).
    CHECK_APPROX_EPS(rmin, rp, rp * 1e-6);
    CHECK_APPROX_EPS(rmax, ra, ra * 1e-6);

    // Planar: every point lies in the plane spanned by two path points through the
    // origin (the focus). Take a normal from two well-separated samples and assert
    // the rest project onto it as ~zero.
    const Vec3d n = path[segs / 4].Cross(path[segs / 2]).Normalized();
    for (const Vec3d& p : path)
        CHECK(std::fabs(p.Dot(n)) < ra * 1e-6);

    // The sampled periapsis matches the body's own periapsis from ElementsToState.
    ecs::OrbitalElements peri = MakeElements(a, e);
    peri.trueAnomaly = 0.0;
    CHECK(std::fabs(sim::ElementsToState(peri, kEarthMu).position.Length() - rp) < 1.0);
}

TEST_CASE(OrbitTrace_HyperbolaIsOpenArcOutsidePeriapsis)
{
    const double a = -2.0e7, e = 1.4; // a<0 hyperbola
    const std::vector<Vec3d> path = SampleOrbitPath(MakeElements(a, e), kEarthMu, 64);
    CHECK_EQ(path.size(), 65u);
    if (path.empty()) return;

    // Open: the endpoints are far apart (both near the asymptotes), not coincident.
    CHECK((path.back() - path.front()).Length() > 1.0e7);

    const double rp = std::fabs(a) * (e - 1.0); // hyperbolic periapsis |a|(e-1)
    for (const Vec3d& p : path)
        CHECK(p.Length() > rp - 1.0); // never inside periapsis
    // The middle sample (nu=0) is periapsis.
    CHECK_APPROX_EPS(path[32].Length(), rp, rp * 1e-6);
}

TEST_CASE(OrbitTrace_DegenerateElementsSampleToNothing)
{
    const ecs::OrbitalElements good = MakeElements(2.0e7, 0.3);
    CHECK(!SampleOrbitPath(good, kEarthMu, 32).empty()); // baseline is non-empty

    CHECK(SampleOrbitPath(good, 0.0, 32).empty());        // mu <= 0
    CHECK(SampleOrbitPath(good, -1.0, 32).empty());       // mu < 0
    CHECK(SampleOrbitPath(good, kEarthMu, 2).empty());    // too few segments

    ecs::OrbitalElements badE = good; badE.eccentricity = -0.1;
    CHECK(SampleOrbitPath(badE, kEarthMu, 32).empty());   // e < 0
    ecs::OrbitalElements badA = good; badA.semiMajorAxis = 0.0;
    CHECK(SampleOrbitPath(badA, kEarthMu, 32).empty());   // a == 0
    ecs::OrbitalElements nanA = good; nanA.semiMajorAxis = std::nan("");
    CHECK(SampleOrbitPath(nanA, kEarthMu, 32).empty());   // non-finite
}
