// =============================================================================
// tests/test_system_instantiate.cpp — the sim, up and going (Stage 12).
//
// The end-to-end proof that the whole astrodynamics stack drives a real world:
// build a star system (Stage 11), instantiate it into live ECS entities, and run
// the FULL shipped StepSimulation orchestrator. Then assert real orbital motion —
// the inner planet sweeps a quarter of its orbit over a quarter of its period
// (Kepler's law, applied by the running sim), the moon keeps orbiting its planet
// (nested rails resolved), and the star holds the origin.
// =============================================================================

#include "test_framework.h"
#include "sim/system_instantiate.h"
#include "sim/star_system.h"
#include "sim/simulation_system.h"
#include "sim/reference_frame.h"

#include "ecs/registry.h"
#include "ecs/components.h"

#include <cmath>

namespace
{
using core::Vec3d;
using namespace sim;

ecs::Entity FindBody(ecs::Registry& r, uint64_t id)
{
    auto* pool = r.GetPool<ecs::GravitationalBody>();
    if (!pool) return ecs::NullEntity;
    for (uint32_t i = 0; i < pool->Count(); ++i)
        if (pool->DataAt(i).bodyId == id)
            return r.EntityAtIndex(pool->EntityAt(i));
    return ecs::NullEntity;
}

Vec3d Pos(ecs::Registry& r, ecs::Entity e)
{
    return r.Get<ecs::Transform>(e).position;
}

double AngleBetween(const Vec3d& a, const Vec3d& b)
{
    const double d = a.Dot(b) / (a.Length() * b.Length());
    return std::acos((std::max)(-1.0, (std::min)(1.0, d)));
}

constexpr double kAU = 1.495978707e11;
constexpr double kPi = 3.14159265358979323846;

} // namespace

TEST_CASE(SceneSeed_StarSystemInstantiatesAndOrbitsUnderFullStep)
{
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
    ecs::Registry registry;

    const StarSystem system = BuildReferenceSystem();
    const uint32_t created = InstantiateStarSystem(registry, root, system);
    CHECK_EQ(created, static_cast<uint32_t>(system.bodies.size()));
    CHECK_EQ(registry.EntityCount(), created);

    const ecs::Entity star   = FindBody(registry, 1);
    const ecs::Entity planet = FindBody(registry, 10);
    const ecs::Entity moon   = FindBody(registry, 11);
    CHECK_FALSE(star.IsNull());
    CHECK_FALSE(planet.IsNull());
    CHECK_FALSE(moon.IsNull());
    if (star.IsNull() || planet.IsNull() || moon.IsNull()) return;

    // Initial placement: star at the frame origin, planet at ~1 AU, moon within a
    // moon-orbit-radius of the planet.
    CHECK(Pos(registry, star).Length() < 1.0);
    CHECK_APPROX_EPS(Pos(registry, planet).Length(), kAU, kAU * 1.0e-6);
    CHECK((Pos(registry, moon) - Pos(registry, planet)).Length() < 4.0e8);
    const Vec3d planetStart = Pos(registry, planet);

    // Drive the FULL orchestrator to a quarter of the inner planet's period.
    const double muSun = 1.32712440018e20;
    const double period = 2.0 * kPi * std::sqrt(kAU * kAU * kAU / muSun);
    const int steps = 40;
    const double dt = (0.25 * period) / steps;

    SimulationStepConfig config;
    config.activeFrame = root;
    config.masterFrame = root;
    config.coordinateTime = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        const SimulationStepResult result =
            StepSimulation(registry, frames, dt, config);
        CHECK(result.accepted);
        config.coordinateTime += dt;
    }

    // The planet swept a QUARTER orbit: a quarter of the period is a 90° arc for a
    // circular orbit. This is the running sim applying Kepler's law, not a builder
    // field read back — the load-bearing "it actually moves correctly" assertion.
    const Vec3d planetNow = Pos(registry, planet);
    CHECK_APPROX_EPS(AngleBetween(planetStart, planetNow), 0.5 * kPi, 1.0e-3);
    CHECK_APPROX_EPS(planetNow.Length(), kAU, kAU * 1.0e-6); // still on its orbit
    CHECK((planetNow - planetStart).Length() > 0.5 * kAU);   // it genuinely moved

    // The moon still orbits its planet at ~its orbit radius (nested rails: the moon
    // resolves against the planet's advanced position each step).
    CHECK((Pos(registry, moon) - planetNow).Length() < 4.0e8);
    CHECK((Pos(registry, moon) - planetNow).Length() > 3.0e8);

    // The star held the origin throughout (sole N-body body, invMass 0, no partners).
    CHECK(Pos(registry, star).Length() < 1.0);
}

TEST_CASE(SceneSeed_InstantiationIsDeterministicAndRejectsMalformed)
{
    // Same system instantiated twice into fresh registries yields identical
    // initial positions (deterministic seeding).
    const StarSystem system = BuildReferenceSystem();
    auto positionsOf = [&](ecs::Registry& r) {
        FrameGraph frames;
        const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
        InstantiateStarSystem(r, root, system);
        Vec3d p{};
        const ecs::Entity planet = FindBody(r, 10);
        if (!planet.IsNull()) p = Pos(r, planet);
        return p;
    };
    ecs::Registry a, b;
    CHECK(positionsOf(a) == positionsOf(b));

    // A malformed system (child before its primary) creates nothing past the bad
    // body: the second entry references a primary that has not been placed.
    StarSystem bad;
    SystemBody child;
    child.bodyId = 5;
    child.primaryBodyId = 99; // never created
    child.owner = ecs::OrbitOwner::OnRails;
    child.mu = 1.0;
    child.hasOrbit = true;
    child.orbit = CircularOrbit(1.0e14, 99, 1.0e7, 0.0, 0.0, 0.0, 0.0);
    bad.bodies.push_back(child);
    ecs::Registry c;
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
    CHECK_EQ(InstantiateStarSystem(c, root, bad), 0u);
}
