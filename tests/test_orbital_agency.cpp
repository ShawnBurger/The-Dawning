// =============================================================================
// tests/test_orbital_agency.cpp — player orbital agency.
//
// Two claims underpin flying the ship on a live Kepler orbit that engine burns
// reshape:
//   1. sim::ResolvePrimaryFor picks the right patched-conics primary for a body
//      at an arbitrary point — Earth for a ship in low orbit, the star for deep
//      space — and excludes the querying body itself. This is what lets the
//      force-integrated ship (untouched by StepSoiTransitions) know whose gravity
//      it is under.
//   2. A prograde burn raises apoapsis and a retrograde burn lowers periapsis,
//      about the burn point, when the new osculating elements are read back with
//      sim::StateToElements — i.e. changing velocity changes the orbit exactly the
//      way the HUD and the trace report.
// =============================================================================

#include "test_framework.h"
#include "sim/soi_transition.h"      // ResolvePrimaryFor
#include "sim/soi.h"                 // SphereOfInfluenceRadius
#include "sim/star_system.h"
#include "sim/system_instantiate.h"
#include "sim/kepler.h"             // StateVector, StateToElements
#include "sim/reference_frame.h"

#include "ecs/registry.h"
#include "ecs/components.h"

#include <cmath>

namespace
{
using core::Vec3d;
using sim::StateVector;

constexpr double kMuSun   = 1.32712440018e20;
constexpr double kMuEarth = 3.986004418e14;
constexpr double kAU      = 1.495978707e11;

Vec3d BodyPosition(ecs::Registry& r, uint64_t id)
{
    auto* pool = r.GetPool<ecs::GravitationalBody>();
    for (uint32_t i = 0; i < pool->Count(); ++i)
        if (pool->DataAt(i).bodyId == id)
            return r.Get<ecs::Transform>(r.EntityAtIndex(pool->EntityAt(i))).position;
    return Vec3d{};
}
} // namespace

TEST_CASE(Agency_ResolvePrimary_ShipInLowOrbitResolvesToEarth)
{
    sim::FrameGraph frames;
    const sim::FrameId root = frames.CreateFrame(sim::kInvalidFrame, sim::WorldPos{});
    ecs::Registry registry;
    sim::InstantiateStarSystem(registry, root, sim::BuildReferenceSystem());

    const Vec3d earth = BodyPosition(registry, 10);
    // 20,000 km above Earth — far inside Earth's SOI (~9.2e8 m) and far outside its
    // radius, so the tightest containing sphere is unambiguously Earth's.
    const Vec3d shipPos = earth + Vec3d{ 0.0, 0.0, 2.0e7 };

    const sim::ResolvedPrimary p = sim::ResolvePrimaryFor(registry, shipPos, /*self*/ 999);
    CHECK(p.found);
    CHECK_EQ(p.bodyId, 10ull);
    CHECK_APPROX_EPS(p.mu, kMuEarth, kMuEarth * 1e-9);
    // The reported primary position is Earth's own, to the metre.
    CHECK((p.position - earth).Length() < 1.0);
}

TEST_CASE(Agency_ResolvePrimary_DeepSpaceResolvesToStar)
{
    sim::FrameGraph frames;
    const sim::FrameId root = frames.CreateFrame(sim::kInvalidFrame, sim::WorldPos{});
    ecs::Registry registry;
    sim::InstantiateStarSystem(registry, root, sim::BuildReferenceSystem());

    // Half an AU out along +X: nowhere near any planet's SOI, so only the star's
    // unbounded sphere contains it.
    const Vec3d deep{ 0.5 * kAU, 0.0, 0.0 };
    const sim::ResolvedPrimary p = sim::ResolvePrimaryFor(registry, deep, /*self*/ 999);
    CHECK(p.found);
    CHECK_EQ(p.bodyId, 1ull);
    CHECK_APPROX_EPS(p.mu, kMuSun, kMuSun * 1e-9);
}

TEST_CASE(Agency_ResolvePrimary_ExcludesSelf)
{
    sim::FrameGraph frames;
    const sim::FrameId root = frames.CreateFrame(sim::kInvalidFrame, sim::WorldPos{});
    ecs::Registry registry;
    sim::InstantiateStarSystem(registry, root, sim::BuildReferenceSystem());

    // Query AT Earth's centre but declare self == Earth: Earth is excluded from the
    // wells (a body is never its own primary), so the next tightest containing sphere
    // — the star's — must win. Without the self-exclusion Earth would capture itself.
    const Vec3d earth = BodyPosition(registry, 10);
    const sim::ResolvedPrimary p = sim::ResolvePrimaryFor(registry, earth, /*self*/ 10);
    CHECK(p.found);
    CHECK_EQ(p.bodyId, 1ull);
}

TEST_CASE(Agency_ProgradeBurnRaisesApoapsisAtTheBurnPoint)
{
    const double mu = kMuEarth;
    const double r  = 2.0e7;               // circular-orbit radius, m
    const double vc = std::sqrt(mu / r);   // circular speed

    // Circular orbit in the equatorial plane: position +X, velocity +Y.
    const StateVector circ{ Vec3d{ r, 0.0, 0.0 }, Vec3d{ 0.0, vc, 0.0 } };
    const ecs::OrbitalElements el0 = sim::StateToElements(circ, mu);
    CHECK(el0.eccentricity < 1e-6);                       // genuinely circular
    CHECK_APPROX_EPS(el0.semiMajorAxis, r, r * 1e-9);     // a == radius

    // Prograde burn: +10% along the velocity direction.
    const StateVector burned{ circ.position, circ.velocity + Vec3d{ 0.0, vc, 0.0 }.Normalized() * (0.1 * vc) };
    const ecs::OrbitalElements el1 = sim::StateToElements(burned, mu);
    const double peri = el1.semiMajorAxis * (1.0 - el1.eccentricity);
    const double apo  = el1.semiMajorAxis * (1.0 + el1.eccentricity);

    CHECK(el1.eccentricity > el0.eccentricity + 0.05);    // orbit became eccentric
    CHECK(apo > r * 1.05);                                // apoapsis rose
    CHECK_APPROX_EPS(peri, r, r * 1e-3);                  // burn point stays periapsis
}

TEST_CASE(Agency_RetrogradeBurnLowersPeriapsisAtTheBurnPoint)
{
    const double mu = kMuEarth;
    const double r  = 2.0e7;
    const double vc = std::sqrt(mu / r);

    const StateVector circ{ Vec3d{ r, 0.0, 0.0 }, Vec3d{ 0.0, vc, 0.0 } };

    // Retrograde burn: -10% along the velocity direction.
    const StateVector burned{ circ.position, circ.velocity - Vec3d{ 0.0, vc, 0.0 }.Normalized() * (0.1 * vc) };
    const ecs::OrbitalElements el = sim::StateToElements(burned, mu);
    const double peri = el.semiMajorAxis * (1.0 - el.eccentricity);
    const double apo  = el.semiMajorAxis * (1.0 + el.eccentricity);

    CHECK(el.eccentricity > 0.05);      // orbit became eccentric
    CHECK(peri < r * 0.95);             // periapsis dropped
    CHECK_APPROX_EPS(apo, r, r * 1e-3); // burn point stays apoapsis
}
