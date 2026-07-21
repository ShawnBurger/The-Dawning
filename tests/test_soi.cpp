// =============================================================================
// tests/test_soi.cpp — sphere-of-influence radii, dominant-SOI membership, the
// hysteresis deadband, and the continuity-preserving repatch (Sim Stage 7).
//
// Invariants (RELATIVISTIC_SIM_ARCHITECTURE.md §4.4/§4.5): physical SOI/Hill
// radii against known Solar-System values; deepest-well membership over a nested
// star→planet→moon system; a hysteresis deadband that suppresses boundary thrash;
// and — the load-bearing one — a repatch that is continuous in GLOBAL position
// AND velocity, which the "subtract the primary velocity" step is watched making.
// =============================================================================

#include "test_framework.h"
#include "sim/soi.h"
#include "sim/nbody.h"    // PromoteFromRails
#include "sim/kepler.h"   // StateVector

#include <cmath>
#include <limits>
#include <vector>

namespace
{
using core::Vec3d;
using namespace sim;

// Gravitational parameters (m^3/s^2) and orbit radii (m).
constexpr double kMuSun   = 1.32712440018e20;
constexpr double kMuEarth = 3.986004418e14;
constexpr double kMuMoon  = 4.9048695e12;
constexpr double kAU        = 1.495978707e11; // Earth semi-major axis
constexpr double kMoonOrbit = 3.844e8;        // Moon semi-major axis

double Len(const Vec3d& v) { return v.Length(); }

} // namespace

TEST_CASE(Soi_LaplaceRadiusMatchesKnownEarthValue)
{
    // Earth's sphere of influence about the Sun is ~9.24e8 m (0.924e6 km). The
    // 2/5 exponent is load-bearing: a 1/3 exponent (the Hill form's) would give
    // ~2.16e9 m, far outside this tolerance.
    const double earth = SphereOfInfluenceRadius(kAU, kMuEarth, kMuSun);
    CHECK_APPROX_EPS(earth, 9.24e8, 1.0e7); // within ~1%

    // Moon's SOI about Earth is ~6.6e7 m.
    const double moon = SphereOfInfluenceRadius(kMoonOrbit, kMuMoon, kMuEarth);
    CHECK_APPROX_EPS(moon, 6.62e7, 1.0e6);

    // Degenerate inputs have no sphere.
    CHECK_EQ(SphereOfInfluenceRadius(0.0, kMuEarth, kMuSun), 0.0);
    CHECK_EQ(SphereOfInfluenceRadius(kAU, -1.0, kMuSun), 0.0);
    CHECK_EQ(SphereOfInfluenceRadius(kAU, kMuEarth, 0.0), 0.0);
    CHECK_EQ(SphereOfInfluenceRadius(
                 std::numeric_limits<double>::infinity(), kMuEarth, kMuSun),
             0.0);
}

TEST_CASE(Soi_HillRadiusMatchesKnownEarthValue)
{
    // Earth's Hill sphere is ~1.496e9 m at e=0.
    const double hill = HillRadius(kAU, 0.0, kMuEarth, kMuSun);
    CHECK_APPROX_EPS(hill, 1.496e9, 1.5e7); // within ~1%

    // The periapsis form shrinks with eccentricity: r_H(e) = r_H(0)·(1−e).
    const double hillE = HillRadius(kAU, 0.25, kMuEarth, kMuSun);
    CHECK_APPROX_EPS(hillE, hill * 0.75, 1.0e3);

    // An unbound arc (e >= 1) or degenerate input has no stable Hill sphere.
    CHECK_EQ(HillRadius(kAU, 1.0, kMuEarth, kMuSun), 0.0);
    CHECK_EQ(HillRadius(kAU, 1.5, kMuEarth, kMuSun), 0.0);
    CHECK_EQ(HillRadius(-1.0, 0.0, kMuEarth, kMuSun), 0.0);
}

namespace
{
// A star→planet→moon system laid out along +x near the origin. Star reaches the
// whole system (infinite sphere) so interplanetary points resolve to it.
std::vector<SoiWell> MakeNestedSystem()
{
    const double earthSoi = SphereOfInfluenceRadius(kAU, kMuEarth, kMuSun);   // ~9.24e8
    const double moonSoi   = SphereOfInfluenceRadius(kMoonOrbit, kMuMoon, kMuEarth); // ~6.6e7
    std::vector<SoiWell> wells;
    wells.push_back(SoiWell{ 1,
        WorldPos::FromOffset({ 0.0, 0.0, 0.0 }),
        std::numeric_limits<double>::infinity() });                 // star
    wells.push_back(SoiWell{ 2,
        WorldPos::FromOffset({ kAU, 0.0, 0.0 }), earthSoi });       // planet
    wells.push_back(SoiWell{ 3,
        WorldPos::FromOffset({ kAU + kMoonOrbit, 0.0, 0.0 }), moonSoi }); // moon
    return wells;
}
} // namespace

TEST_CASE(Soi_ResolveDominantPicksTightestContainingWell)
{
    const std::vector<SoiWell> wells = MakeNestedSystem();

    // Inside the planet SOI but far from the moon -> planet (2).
    CHECK_EQ(ResolveDominantSoi(
                 WorldPos::FromOffset({ kAU + 1.0e7, 0.0, 0.0 }), wells), 2ull);

    // Inside the moon SOI (and necessarily the planet's) -> tightest = moon (3).
    CHECK_EQ(ResolveDominantSoi(
                 WorldPos::FromOffset({ kAU + kMoonOrbit + 1.0e7, 0.0, 0.0 }),
                 wells), 3ull);

    // Interplanetary: outside every planet/moon sphere, inside the star's -> star.
    CHECK_EQ(ResolveDominantSoi(
                 WorldPos::FromOffset({ 5.0e11, 0.0, 0.0 }), wells), 1ull);

    // With a finite (zero) root sphere instead, the same interplanetary point is
    // inside NO sphere -> kInvalidSoi (the implicit interstellar root).
    std::vector<SoiWell> finiteRoot = wells;
    finiteRoot[0].soiRadius = 0.0;
    CHECK_EQ(ResolveDominantSoi(
                 WorldPos::FromOffset({ 5.0e11, 0.0, 0.0 }), finiteRoot),
             kInvalidSoi);
}

TEST_CASE(Soi_ResolveDominantIsDeterministicOnTies)
{
    // Two sibling wells, equal depth and equal radius, both containing the point:
    // the smaller bodyId wins, regardless of vector order.
    const WorldPos c = WorldPos::FromOffset({ 100.0, 0.0, 0.0 });
    SoiWell a{ 20, c, 50.0 };
    SoiWell b{ 10, c, 50.0 };
    CHECK_EQ(ResolveDominantSoi(c, std::vector<SoiWell>{ a, b }), 10ull);
    CHECK_EQ(ResolveDominantSoi(c, std::vector<SoiWell>{ b, a }), 10ull);
}

TEST_CASE(Soi_HysteresisOpensADeadbandBothWays)
{
    const std::vector<SoiWell> wells = MakeNestedSystem();
    const double earthSoi = wells[1].soiRadius; // ~9.24e8

    // (A) LEAVING: a point just OUTSIDE the true planet sphere (by ~0.6%). With no
    // hysteresis it resolves to the star; with the planet as current primary and a
    // 2% deadband, the inflated planet sphere still contains it -> stays planet.
    const WorldPos justOutside =
        WorldPos::FromOffset({ kAU + earthSoi * 1.006, 0.0, 0.0 });
    CHECK_EQ(ResolveDominantSoi(justOutside, wells), 1ull);                    // star
    CHECK_EQ(ResolveSoiWithHysteresis(justOutside, wells, /*current=*/2, 0.0),
             1ull);                                                            // no deadband -> flips
    CHECK_EQ(ResolveSoiWithHysteresis(justOutside, wells, /*current=*/2, 0.02),
             2ull);                                                            // sticky -> stays planet

    // (B) ENTERING: a point just INSIDE the true planet sphere (by ~0.6%). With no
    // hysteresis it resolves to the planet; approaching from the star with a 2%
    // deadband the deflated planet sphere excludes it -> not yet entered (star).
    const WorldPos justInside =
        WorldPos::FromOffset({ kAU + earthSoi * 0.994, 0.0, 0.0 });
    CHECK_EQ(ResolveDominantSoi(justInside, wells), 2ull);                     // planet
    CHECK_EQ(ResolveSoiWithHysteresis(justInside, wells, /*current=*/1, 0.02),
             1ull);                                                            // not yet entered -> star
}

TEST_CASE(Soi_RepatchIsContinuousInGlobalPositionAndVelocity)
{
    // Primary (a planet) at an arbitrary GLOBAL position with its own velocity.
    const WorldPos primaryPos =
        WorldPos::FromOffset({ 1.0e11, 2.0e11, -3.0e11 });
    const Vec3d primaryVel{ 5000.0, -3000.0, 1000.0 };

    // A well-conditioned bound orbit (moderate e, inclined) so the Demote/Promote
    // element round-trip is tight. r = 2e7 m; circular speed ~4464 m/s, so this
    // sub-circular speed is elliptical, and the z-component gives it inclination.
    const Vec3d relPos{ 2.0e7, 0.0, 0.0 };
    const Vec3d relVel{ 0.0, 1200.0, 300.0 };
    const WorldPos bodyPos = Translate(primaryPos, relPos);
    const Vec3d    bodyVel = primaryVel + relVel;

    const double now = 100.0;
    const ecs::OrbitState orbit =
        Repatch(bodyPos, bodyVel, primaryPos, primaryVel, kMuEarth,
                /*newPrimaryBodyId=*/2, now);

    CHECK_EQ(orbit.primaryBodyId, 2ull);
    CHECK_EQ(orbit.epoch, now);
    CHECK_EQ(orbit.primaryMu, kMuEarth);

    // Reconstruct the GLOBAL state from the fitted elements (epoch == now, so no
    // advance) and add the primary's own state back.
    const StateVector sv = PromoteFromRails(orbit, 0.0);
    const WorldPos reconPos = Translate(primaryPos, sv.position);
    const Vec3d    reconVel = primaryVel + sv.velocity;

    // Position AND velocity continuous in the global frame, to the tolerance the
    // analytic round-trip actually achieves (relative ~1e-9 here).
    const double posErr = Len(Separation(bodyPos, reconPos));
    const double velErr = Len(bodyVel - reconVel);
    CHECK(posErr < 1.0e-3);        // vs |relPos| = 2e7  -> ~5e-11 relative
    CHECK(velErr < 1.0e-6);        // vs |relVel| ~ 1237 -> ~8e-10 relative
}
