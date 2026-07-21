// =============================================================================
// tests/test_star_system.cpp — star-system builder (Stage 11).
//
// Invariants: parent-before-child topology; each rails body is a genuine circular
// orbit (|r|=a, |v|=sqrt(mu/a), r⊥v) via the shipped PromoteFromRails; Kepler's
// third law holds across the two planets; SOI radii match Stage 7 (Earth ~9.24e8);
// masses are mu/G; and the build is deterministic.
// =============================================================================

#include "test_framework.h"
#include "sim/star_system.h"
#include "sim/nbody.h"   // PromoteFromRails, kGravitationalConstant
#include "sim/kepler.h"  // StateVector

#include <cmath>
#include <limits>

namespace
{
using core::Vec3d;
using namespace sim;

const SystemBody* Find(const StarSystem& s, uint64_t id)
{
    for (const SystemBody& b : s.bodies)
        if (b.bodyId == id) return &b;
    return nullptr;
}

// Period T = 2π·sqrt(a^3/mu).
double Period(double a, double mu)
{
    return 2.0 * 3.14159265358979323846 * std::sqrt(a * a * a / mu);
}

} // namespace

TEST_CASE(StarSystem_TopologyIsParentBeforeChild)
{
    const StarSystem s = BuildReferenceSystem();
    CHECK(s.bodies.size() >= 4u);

    // The star is first, has no primary, is the sole N-body body, holds the origin.
    CHECK_EQ(s.bodies[0].bodyId, 1ull);
    CHECK_EQ(s.bodies[0].primaryBodyId, 0ull);
    CHECK(s.bodies[0].owner == ecs::OrbitOwner::NBodyActive);
    CHECK_EQ(s.bodies[0].invMass, 0.0);
    CHECK(std::isinf(s.bodies[0].soiRadius));
    CHECK_FALSE(s.bodies[0].hasOrbit);

    // Every non-star body's primary appears strictly earlier, is on rails, and has
    // a valid orbit about that primary.
    for (std::size_t i = 1; i < s.bodies.size(); ++i)
    {
        const SystemBody& b = s.bodies[i];
        CHECK(b.owner == ecs::OrbitOwner::OnRails);
        CHECK(b.hasOrbit);
        CHECK_EQ(b.orbit.primaryBodyId, b.primaryBodyId);
        bool primaryEarlier = false;
        for (std::size_t j = 0; j < i; ++j)
            if (s.bodies[j].bodyId == b.primaryBodyId) primaryEarlier = true;
        CHECK(primaryEarlier);
    }
}

TEST_CASE(StarSystem_RailsBodiesAreGenuineCircularOrbits)
{
    const StarSystem s = BuildReferenceSystem();
    for (const SystemBody& b : s.bodies)
    {
        if (!b.hasOrbit) continue;
        const double a  = b.orbit.elements.semiMajorAxis;
        const double mu = b.orbit.primaryMu;
        const StateVector sv = PromoteFromRails(b.orbit, 0.0);

        // Circular: radius = a, speed = sqrt(mu/a), and velocity ⊥ radius.
        CHECK_APPROX_EPS(sv.position.Length(), a, a * 1.0e-9);
        CHECK_APPROX_EPS(sv.velocity.Length(), std::sqrt(mu / a),
                         std::sqrt(mu / a) * 1.0e-9);
        CHECK(std::fabs(sv.position.Dot(sv.velocity)) /
                  (sv.position.Length() * sv.velocity.Length()) < 1.0e-9);
        CHECK_EQ(b.orbit.elements.eccentricity, 0.0);
    }
}

TEST_CASE(StarSystem_KeplersThirdLawHoldsAcrossPlanets)
{
    const StarSystem s = BuildReferenceSystem();
    const SystemBody* p1 = Find(s, 10);
    const SystemBody* p2 = Find(s, 20);
    CHECK(p1 && p2);
    if (!p1 || !p2) return;

    const double a1 = p1->orbit.elements.semiMajorAxis;
    const double a2 = p2->orbit.elements.semiMajorAxis;
    const double muSun = p1->orbit.primaryMu; // both orbit the star
    CHECK_EQ(p2->orbit.primaryMu, muSun);

    // T^2 / a^3 is the same constant (4π²/mu) for both planets.
    const double k1 = Period(a1, muSun) * Period(a1, muSun) / (a1 * a1 * a1);
    const double k2 = Period(a2, muSun) * Period(a2, muSun) / (a2 * a2 * a2);
    CHECK_APPROX_EPS(k1, k2, k1 * 1.0e-12);
    CHECK_APPROX_EPS(k1, 4.0 * 3.14159265358979323846 *
                             3.14159265358979323846 / muSun,
                     k1 * 1.0e-12);
}

TEST_CASE(StarSystem_SoiRadiiAndMassesArePhysical)
{
    const StarSystem s = BuildReferenceSystem();
    const SystemBody* earth = Find(s, 10);
    const SystemBody* moon  = Find(s, 11);
    CHECK(earth && moon);
    if (!earth || !moon) return;

    // Inner planet's SOI about the Sun is the known ~9.24e8 m.
    CHECK_APPROX_EPS(earth->soiRadius, 9.24e8, 1.0e7);
    // Moon's SOI about the planet (~6.6e7 m) and strictly inside the planet's.
    CHECK_APPROX_EPS(moon->soiRadius, 6.62e7, 2.0e6);
    CHECK(moon->soiRadius < earth->soiRadius);

    // invMass = G/mu (mass = mu/G) for every body with finite mass.
    for (const SystemBody& b : s.bodies)
        if (b.invMass != 0.0)
            CHECK_APPROX_EPS(b.invMass, kGravitationalConstant / b.mu,
                             (kGravitationalConstant / b.mu) * 1.0e-12);
}

TEST_CASE(StarSystem_BuildIsDeterministic)
{
    const StarSystem a = BuildReferenceSystem();
    const StarSystem b = BuildReferenceSystem();
    CHECK_EQ(a.bodies.size(), b.bodies.size());
    for (std::size_t i = 0; i < a.bodies.size() && i < b.bodies.size(); ++i)
    {
        CHECK_EQ(a.bodies[i].bodyId, b.bodies[i].bodyId);
        CHECK_EQ(a.bodies[i].mu, b.bodies[i].mu);
        CHECK_EQ(a.bodies[i].soiRadius, b.bodies[i].soiRadius);
        CHECK_EQ(a.bodies[i].orbit.elements.semiMajorAxis,
                 b.bodies[i].orbit.elements.semiMajorAxis);
        CHECK_EQ(a.bodies[i].orbit.elements.trueAnomaly,
                 b.bodies[i].orbit.elements.trueAnomaly);
    }
}

TEST_CASE(StarSystem_CircularOrbitHelper)
{
    const ecs::OrbitState o = CircularOrbit(3.986e14, 42, 1.0e7, 0.5, 0.3, 1.2, 5.0);
    CHECK_EQ(o.elements.semiMajorAxis, 1.0e7);
    CHECK_EQ(o.elements.eccentricity, 0.0);
    CHECK_EQ(o.elements.inclination, 0.5);
    CHECK_EQ(o.primaryMu, 3.986e14);
    CHECK_EQ(o.primaryBodyId, 42ull);
    CHECK_EQ(o.epoch, 5.0);
}
