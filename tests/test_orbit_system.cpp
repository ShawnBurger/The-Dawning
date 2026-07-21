// =============================================================================
// tests/test_orbit_system.cpp - passive orbital ECS adapter
// =============================================================================

#include "test_framework.h"
#include "sim/orbit_system.h"
#include "sim/nbody.h"

#include <cmath>
#include <limits>
#include <memory>

namespace
{
using core::Vec3d;
using namespace sim;

ecs::Entity MakeOrbitalBody(ecs::Registry& registry,
                            FrameId frame,
                            uint64_t bodyId,
                            ecs::OrbitOwner owner,
                            const Vec3d& position,
                            const Vec3d& velocity,
                            double mu,
                            double radius,
                            double invMass,
                            bool isSource = true)
{
    const ecs::Entity entity = registry.Create();
    ecs::Transform transform;
    transform.position = position;
    registry.Assign<ecs::Transform>(entity, transform);

    ecs::RigidBody body;
    body.linearVelocity = velocity;
    body.invMass = invMass;
    body.prevPosition = position;
    registry.Assign<ecs::RigidBody>(entity, body);
    registry.Assign<ecs::SpatialFrame>(
        entity, ecs::SpatialFrame{ static_cast<uint32_t>(frame) });

    ecs::GravitationalBody gravity;
    gravity.bodyId = bodyId;
    gravity.owner = owner;
    gravity.mu = mu;
    gravity.radius = radius;
    gravity.isSource = isSource;
    registry.Assign<ecs::GravitationalBody>(entity, gravity);
    return entity;
}

ecs::Entity FindBody(ecs::Registry& registry, uint64_t bodyId)
{
    auto* pool = registry.GetPool<ecs::GravitationalBody>();
    if (!pool)
        return ecs::NullEntity;
    for (uint32_t i = 0; i < pool->Count(); ++i)
        if (pool->DataAt(i).bodyId == bodyId)
            return registry.EntityAtIndex(pool->EntityAt(i));
    return ecs::NullEntity;
}

bool Near(double actual, double expected, double tolerance = 1.0e-10)
{
    return std::fabs(actual - expected) <= tolerance;
}

void CheckVecNear(const Vec3d& actual, const Vec3d& expected,
                  double tolerance = 1.0e-10)
{
    CHECK(Near(actual.x, expected.x, tolerance));
    CHECK(Near(actual.y, expected.y, tolerance));
    CHECK(Near(actual.z, expected.z, tolerance));
}

} // namespace

TEST_CASE(OrbitSystem_CrossFrameNBodyMatchesShippedKernel)
{
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(
        kInvalidFrame, WorldPos{ 700, -20, 5, Vec3d{ 100.0, 200.0, 300.0 } });
    const FrameId child = frames.CreateFrame(
        root, Translate(frames.GetFrame(root).origin, Vec3d{ 100.0, 0.0, 0.0 }),
        Vec3d{ 0.0, 2.0, 0.0 });

    ecs::Registry registry;
    const ecs::Entity primary = MakeOrbitalBody(
        registry, root, 10, ecs::OrbitOwner::NBodyActive,
        Vec3d{}, Vec3d{}, 4.0e5, 0.0, 0.0);
    const ecs::Entity orbiter = MakeOrbitalBody(
        registry, child, 20, ecs::OrbitOwner::NBodyActive,
        Vec3d{}, Vec3d{ 0.0, 61.0, 0.0 }, 1.0, 0.0, 1.0);

    std::vector<NBodyParticle> expected(2);
    expected[0] = NBodyParticle{
        Vec3d{}, Vec3d{}, 4.0e5, SofteningLength(4.0e5, 0.0),
        0.0, 10, true,
    };
    expected[1] = NBodyParticle{
        Vec3d{ 100.0, 0.0, 0.0 }, Vec3d{ 0.0, 63.0, 0.0 },
        1.0, SofteningLength(1.0, 0.0), 0.0, 20, true,
    };
    CloseEncounterReport expectedCollisions;
    StepNBodyCollisional(
        expected, 0.01, CloseEncounterConfig{}, expectedCollisions);
    CHECK(expectedCollisions.events.empty());

    const PassiveOrbitStepResult result = StepPassiveOrbits(
        registry, frames, root, 12.0, 0.01);
    CHECK(result.accepted);
    CHECK_EQ(result.nbodyBodyCount, 2u);
    CHECK_EQ(result.railBodyCount, 0u);
    CHECK_EQ(result.destroyedEntityCount, 0u);

    const auto& primaryTransform = registry.Get<ecs::Transform>(primary);
    const auto& primaryBody = registry.Get<ecs::RigidBody>(primary);
    CHECK(primaryTransform.position == expected[0].position);
    CHECK(primaryBody.linearVelocity == expected[0].velocity);

    const Body expectedChild = frames.Reparent(
        Body{ root, expected[1].position, expected[1].velocity }, child);
    const auto& childTransform = registry.Get<ecs::Transform>(orbiter);
    const auto& childBody = registry.Get<ecs::RigidBody>(orbiter);
    CHECK(childTransform.position == expectedChild.localPos);
    CHECK(childBody.linearVelocity == expectedChild.localVel);
}

TEST_CASE(OrbitSystem_RailsResolvePrimaryAndNeverMoveForceOwner)
{
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(
        kInvalidFrame, WorldPos{ 50, 60, 70, Vec3d{ 10.0, 20.0, 30.0 } },
        Vec3d{ 4.0, 5.0, 6.0 });

    ecs::Registry registry;
    const Vec3d primaryPosition{ 15.0, 1.0, -2.0 };
    const Vec3d primaryVelocity{ 2.0, 3.0, 4.0 };
    const ecs::Entity primary = MakeOrbitalBody(
        registry, root, 100, ecs::OrbitOwner::ForceIntegrated,
        primaryPosition, primaryVelocity, 8.0e5, 10.0, 0.25);
    const ecs::Entity satellite = MakeOrbitalBody(
        registry, root, 200, ecs::OrbitOwner::OnRails,
        Vec3d{ 115.0, 1.0, -2.0 }, primaryVelocity,
        1.0, 1.0, 1.0);

    ecs::OrbitState orbit;
    orbit.elements.semiMajorAxis = 100.0;
    orbit.elements.eccentricity = 0.0;
    orbit.primaryMu = 8.0e5;
    orbit.primaryBodyId = 100;
    orbit.epoch = 20.0;
    registry.Assign<ecs::OrbitState>(satellite, orbit);

    const double dt = 0.125;
    const StateVector relative = PromoteFromRails(orbit, dt);
    const PassiveOrbitStepResult result = StepPassiveOrbits(
        registry, frames, root, orbit.epoch, dt);
    CHECK(result.accepted);
    CHECK_EQ(result.nbodyBodyCount, 0u);
    CHECK_EQ(result.railBodyCount, 1u);

    CHECK(registry.Get<ecs::Transform>(primary).position == primaryPosition);
    CHECK(registry.Get<ecs::RigidBody>(primary).linearVelocity == primaryVelocity);
    CheckVecNear(registry.Get<ecs::Transform>(satellite).position,
                 primaryPosition + relative.position);
    CheckVecNear(registry.Get<ecs::RigidBody>(satellite).linearVelocity,
                 primaryVelocity + relative.velocity);
}

TEST_CASE(OrbitSystem_CollisionMergeReconcilesAndDestroysAbsorbedEntity)
{
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
    ecs::Registry registry;
    const ecs::Entity survivor = MakeOrbitalBody(
        registry, root, 10, ecs::OrbitOwner::NBodyActive,
        Vec3d{ -0.5, 0.0, 0.0 }, Vec3d{ 1.0, 0.0, 0.0 },
        4.0, 1.0, 0.5);
    const ecs::Entity absorbed = MakeOrbitalBody(
        registry, root, 20, ecs::OrbitOwner::NBodyActive,
        Vec3d{ 0.5, 0.0, 0.0 }, Vec3d{ -1.0, 0.0, 0.0 },
        6.0, 1.0, 0.25);

    CloseEncounterConfig config;
    config.maxLevel = 0;
    const PassiveOrbitStepResult result = StepPassiveOrbits(
        registry, frames, root, 0.0, 0.001, config);
    CHECK(result.accepted);
    CHECK_EQ(result.destroyedEntityCount, 1u);
    CHECK_EQ(result.collisions.events.size(), static_cast<size_t>(1));
    CHECK(result.collisions.events[0].merged);
    CHECK_EQ(result.collisions.events[0].survivorId, 10ull);
    CHECK(registry.IsAlive(survivor));
    CHECK_FALSE(registry.IsAlive(absorbed));
    CHECK_EQ(registry.EntityCount(), 1u);
    CHECK_EQ(registry.Get<ecs::GravitationalBody>(survivor).mu, 10.0);
    CHECK(Near(registry.Get<ecs::RigidBody>(survivor).invMass, 1.0 / 6.0));
}

TEST_CASE(OrbitSystem_ForceIntegratedIsExcludedAndNegativeControlMoves)
{
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
    ecs::Registry registry;
    const ecs::Entity ship = MakeOrbitalBody(
        registry, root, 42, ecs::OrbitOwner::ForceIntegrated,
        Vec3d{ 12.0, 3.0, -4.0 }, Vec3d{ 9.0, 0.0, 0.0 },
        1.0, 0.0, 1.0, false);
    const Vec3d before = registry.Get<ecs::Transform>(ship).position;

    NBodyParticle wrong;
    wrong.position = before;
    wrong.velocity = registry.Get<ecs::RigidBody>(ship).linearVelocity;
    wrong.mu = 1.0;
    wrong.softening = SofteningLength(1.0, 0.0);
    wrong.bodyId = 42;
    std::vector<NBodyParticle> wrongLane{ wrong };
    StepNBody(wrongLane, 1.0);
    CHECK(wrongLane[0].position != before);

    const PassiveOrbitStepResult result = StepPassiveOrbits(
        registry, frames, root, 0.0, 1.0);
    CHECK(result.accepted);
    CHECK_EQ(result.nbodyBodyCount, 0u);
    CHECK_EQ(result.railBodyCount, 0u);
    CHECK(registry.Get<ecs::Transform>(ship).position == before);
}

TEST_CASE(OrbitSystem_InvalidInputIsAtomic)
{
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
    ecs::Registry registry;
    const ecs::Entity a = MakeOrbitalBody(
        registry, root, 1, ecs::OrbitOwner::NBodyActive,
        Vec3d{ -10.0, 0.0, 0.0 }, Vec3d{}, 100.0, 0.0, 1.0);
    const ecs::Entity b = MakeOrbitalBody(
        registry, root, 1, ecs::OrbitOwner::NBodyActive,
        Vec3d{ 10.0, 0.0, 0.0 }, Vec3d{}, 100.0, 0.0, 1.0);
    const ecs::Transform beforeA = registry.Get<ecs::Transform>(a);
    const ecs::Transform beforeB = registry.Get<ecs::Transform>(b);

    CHECK_FALSE(StepPassiveOrbits(registry, frames, root, 0.0, 0.1).accepted);
    CHECK(registry.Get<ecs::Transform>(a).position == beforeA.position);
    CHECK(registry.Get<ecs::Transform>(b).position == beforeB.position);
    CHECK(registry.IsAlive(a));
    CHECK(registry.IsAlive(b));

    registry.Get<ecs::GravitationalBody>(b).bodyId = 2;
    CloseEncounterConfig invalid;
    invalid.eta = (std::numeric_limits<double>::quiet_NaN)();
    CHECK_FALSE(StepPassiveOrbits(
        registry, frames, root, 0.0, 0.1, invalid).accepted);
    CHECK(registry.Get<ecs::Transform>(a).position == beforeA.position);
    CHECK(registry.Get<ecs::Transform>(b).position == beforeB.position);
}

TEST_CASE(OrbitSystem_AbsorbedPrimaryDoesNotFreezeAndRepointsToSurvivor)
{
    // Regression for the HIGH review finding: an on-rails body whose primary is
    // absorbed as a NON-survivor by a same-step merge must not stall the whole
    // step. Survivor id == min(clusterIds), so a planet (id 100) merged with a
    // lower-id asteroid (id 50) is absorbed while the moon (id 200) orbits 100.
    // Before the fix, resolveRail(100) failed -> the entire StepPassiveOrbits
    // returned an unaccepted default with nothing committed, and every later step
    // reproduced the identical merge and rejection: a permanent freeze.
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
    ecs::Registry registry;

    // Asteroid (lower id) survives; planet (higher id) is absorbed.
    const ecs::Entity asteroid = MakeOrbitalBody(
        registry, root, 50, ecs::OrbitOwner::NBodyActive,
        Vec3d{ -0.5, 0.0, 0.0 }, Vec3d{ 1.0, 0.0, 0.0 }, 4.0, 1.0, 0.5);
    const ecs::Entity planet = MakeOrbitalBody(
        registry, root, 100, ecs::OrbitOwner::NBodyActive,
        Vec3d{ 0.5, 0.0, 0.0 }, Vec3d{ -1.0, 0.0, 0.0 }, 6.0, 1.0, 0.25);

    // Moon on analytic rails around the planet (id 100).
    const ecs::Entity moon = MakeOrbitalBody(
        registry, root, 200, ecs::OrbitOwner::OnRails,
        Vec3d{ 100.5, 0.0, 0.0 }, Vec3d{ 0.0, 0.0, 0.0 },
        1.0, 0.0, 1.0, /*isSource=*/false);
    ecs::OrbitState orbit;
    orbit.elements.semiMajorAxis = 100.0;
    orbit.elements.eccentricity = 0.0;
    orbit.primaryMu = 6.0;
    orbit.primaryBodyId = 100;
    orbit.epoch = 0.0;
    registry.Assign<ecs::OrbitState>(moon, orbit);

    CloseEncounterConfig config;
    config.maxLevel = 0; // force the merge at the top level this step

    // STEP 1 — the merge absorbs the moon's primary. The step must be accepted.
    const PassiveOrbitStepResult step1 = StepPassiveOrbits(
        registry, frames, root, 0.0, 0.001, config);
    CHECK(step1.accepted);
    CHECK_EQ(step1.destroyedEntityCount, 1u);
    CHECK_EQ(step1.collisions.events.size(), static_cast<size_t>(1));
    if (!step1.collisions.events.empty()) // guard: broken build returns 0 events
    {
        CHECK(step1.collisions.events[0].merged);
        CHECK_EQ(step1.collisions.events[0].survivorId, 50ull);
    }
    CHECK(registry.IsAlive(asteroid));
    CHECK_FALSE(registry.IsAlive(planet));
    CHECK(registry.IsAlive(moon));
    // The moon's stored primary is repointed to the survivor so later steps resolve.
    CHECK_EQ(registry.Get<ecs::OrbitState>(moon).primaryBodyId, 50ull);
    // The moon was actually propagated to a finite world-consistent position.
    const Vec3d moonPos = registry.Get<ecs::Transform>(moon).position;
    CHECK(std::isfinite(moonPos.x) && std::isfinite(moonPos.y) &&
          std::isfinite(moonPos.z));

    // STEP 2 — the absorbed primary (100) no longer exists. With the repoint the
    // moon resolves against the survivor (50); WITHOUT it, primaryBodyId is still
    // 100, recordById has no 100, resolveRail returns false, and the step freezes
    // again. This second step is the watched failure for the repoint half.
    // Guarded on step1.accepted: if step 1 was (wrongly) rejected, the two
    // overlapping bodies remain and a default-config resolve would churn — the
    // freeze this test exists to forbid, so there is nothing to assert past it.
    if (step1.accepted)
    {
        const PassiveOrbitStepResult step2 = StepPassiveOrbits(
            registry, frames, root, 0.001, 0.001);
        CHECK(step2.accepted);
        CHECK_EQ(step2.railBodyCount, 1u);
        CHECK(registry.IsAlive(moon));
    }
}

TEST_CASE(OrbitSystem_ThreeBodyMergeSumsInertialMass)
{
    // Exercises the merge mass-sum path that the determinism finding hardened
    // (each group's members are now sorted by bodyId before summing). Three
    // NBodyActive bodies collapse into one survivor; the survivor's inertial mass
    // must equal the exact sum of the three. NOTE: masses are exact powers of two,
    // so the sum is order-independent by construction; the sort's benefit is
    // toolchain PORTABILITY of the low-order rounding for non-dyadic masses, which
    // is not observable from a single MSVC build (bucket order is fixed per build,
    // like the logger race). This is behavioral coverage of the path, not a
    // watched-failing witness for the sort.
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});
    ecs::Registry registry;
    MakeOrbitalBody(registry, root, 10, ecs::OrbitOwner::NBodyActive,
                    Vec3d{ -0.6, 0.0, 0.0 }, Vec3d{ 1.0, 0.0, 0.0 },
                    2.0, 1.0, 0.5);   // mass 2
    MakeOrbitalBody(registry, root, 20, ecs::OrbitOwner::NBodyActive,
                    Vec3d{ 0.0, 0.0, 0.0 }, Vec3d{ 0.0, 0.0, 0.0 },
                    4.0, 1.0, 0.25);  // mass 4
    MakeOrbitalBody(registry, root, 30, ecs::OrbitOwner::NBodyActive,
                    Vec3d{ 0.6, 0.0, 0.0 }, Vec3d{ -1.0, 0.0, 0.0 },
                    8.0, 1.0, 0.125); // mass 8

    CloseEncounterConfig config;
    config.maxLevel = 0;
    const PassiveOrbitStepResult result = StepPassiveOrbits(
        registry, frames, root, 0.0, 0.001, config);
    CHECK(result.accepted);
    CHECK_EQ(result.destroyedEntityCount, 2u);
    const ecs::Entity survivor = FindBody(registry, 10);
    CHECK_FALSE(survivor.IsNull());
    // Combined inertial mass 2 + 4 + 8 = 14 -> invMass 1/14.
    CHECK(Near(registry.Get<ecs::RigidBody>(survivor).invMass, 1.0 / 14.0));
}

TEST_CASE(OrbitSystem_DeterministicAcrossComponentInsertionOrder)
{
    FrameGraph frames;
    const FrameId root = frames.CreateFrame(kInvalidFrame, WorldPos{});

    auto build = [&](bool reverse) {
        auto registry = std::make_unique<ecs::Registry>();
        auto add = [&](uint64_t id, double x, double vy, double mu) {
            MakeOrbitalBody(*registry, root, id, ecs::OrbitOwner::NBodyActive,
                            Vec3d{ x, 0.0, 0.0 }, Vec3d{ 0.0, vy, 0.0 },
                            mu, 0.0, 1.0);
        };
        if (reverse)
        {
            add(30, 120.0, -10.0, 2.0);
            add(20, 60.0, 30.0, 3.0);
            add(10, 0.0, 0.0, 4.0e5);
        }
        else
        {
            add(10, 0.0, 0.0, 4.0e5);
            add(20, 60.0, 30.0, 3.0);
            add(30, 120.0, -10.0, 2.0);
        }
        return registry;
    };

    auto forward = build(false);
    auto reverse = build(true);
    CHECK(StepPassiveOrbits(*forward, frames, root, 0.0, 0.01).accepted);
    CHECK(StepPassiveOrbits(*reverse, frames, root, 0.0, 0.01).accepted);
    for (uint64_t id : { 10ull, 20ull, 30ull })
    {
        const ecs::Entity a = FindBody(*forward, id);
        const ecs::Entity b = FindBody(*reverse, id);
        CHECK_FALSE(a.IsNull());
        CHECK_FALSE(b.IsNull());
        CHECK(forward->Get<ecs::Transform>(a).position ==
              reverse->Get<ecs::Transform>(b).position);
        CHECK(forward->Get<ecs::RigidBody>(a).linearVelocity ==
              reverse->Get<ecs::RigidBody>(b).linearVelocity);
    }
}
