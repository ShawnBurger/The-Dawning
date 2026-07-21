// =============================================================================
// tests/test_relativity_system.cpp - ECS proper-time adapter
// =============================================================================

#include "test_framework.h"
#include "sim/relativity_system.h"

#include <limits>

namespace
{

ecs::Entity MakeClockBody(ecs::Registry& registry, sim::FrameId frame,
                          const core::Vec3d& position,
                          const core::Vec3d& velocity)
{
    const ecs::Entity entity = registry.Create();
    ecs::Transform transform;
    transform.position = position;
    registry.Assign<ecs::Transform>(entity, transform);
    ecs::RigidBody body;
    body.linearVelocity = velocity;
    body.invMass = 1.0;
    registry.Assign<ecs::RigidBody>(entity, body);
    registry.Assign<ecs::SpatialFrame>(
        entity, ecs::SpatialFrame{ static_cast<uint32_t>(frame) });
    registry.Assign<ecs::RelativisticClock>(
        entity, ecs::RelativisticClock{});
    return entity;
}

} // namespace

TEST_CASE(RelativitySystem_MasterFrameAndPrimaryDriveClock)
{
    sim::FrameGraph frames;
    const sim::FrameId master = frames.CreateFrame(
        sim::kInvalidFrame, sim::WorldPos{});
    const sim::FrameId moving = frames.CreateFrame(
        master, sim::WorldPos::FromOffset(core::Vec3d{ 1000.0, 0.0, 0.0 }),
        core::Vec3d{ 30000.0, 0.0, 0.0 });

    ecs::Registry registry;
    const ecs::Entity source = registry.Create();
    registry.Assign<ecs::Transform>(source, ecs::Transform{});
    registry.Assign<ecs::SpatialFrame>(
        source, ecs::SpatialFrame{ static_cast<uint32_t>(master) });
    ecs::GravitationalBody gravity;
    gravity.bodyId = 10;
    gravity.mu = 3.986004418e14;
    gravity.radius = 6.371e6;
    registry.Assign<ecs::GravitationalBody>(source, gravity);

    const ecs::Entity traveler = MakeClockBody(
        registry, moving, core::Vec3d{}, core::Vec3d{});
    const sim::ClockGravityBinding binding{ traveler, 10 };
    constexpr double dt = 2.0;
    const double beta = 30000.0 / sim::kSpeedOfLight;
    const double distance = 1000.0;
    const double expectedMinusOne = sim::CombinedDilationFactorMinusOne(
        beta, gravity.mu, distance,
        sim::SofteningLength(gravity.mu, gravity.radius));

    const sim::RelativisticClockStepResult result =
        sim::StepRelativisticClocks(
            registry, frames, master, dt,
            std::span<const sim::ClockGravityBinding>(&binding, 1));
    CHECK(result.accepted);
    CHECK_EQ(result.clockCount, 1u);
    CHECK_EQ(result.gravitationalClockCount, 1u);
    const ecs::RelativisticClock& clock =
        registry.Get<ecs::RelativisticClock>(traveler);
    CHECK_EQ(clock.coordinateTime, dt);
    CHECK_EQ(clock.properTimeDeviation, dt * expectedMinusOne);
    CHECK(sim::ProperTime(clock) < clock.coordinateTime);
}

TEST_CASE(RelativitySystem_InvalidBindingIsAtomic)
{
    sim::FrameGraph frames;
    const sim::FrameId master = frames.CreateFrame(
        sim::kInvalidFrame, sim::WorldPos{});
    ecs::Registry registry;
    const ecs::Entity a = MakeClockBody(
        registry, master, core::Vec3d{}, core::Vec3d{ 1.0, 0.0, 0.0 });
    const ecs::Entity b = MakeClockBody(
        registry, master, core::Vec3d{ 1.0, 0.0, 0.0 }, core::Vec3d{});
    const ecs::RelativisticClock beforeA =
        registry.Get<ecs::RelativisticClock>(a);
    const ecs::RelativisticClock beforeB =
        registry.Get<ecs::RelativisticClock>(b);
    const sim::ClockGravityBinding missing{ b, 999 };

    CHECK_FALSE(sim::StepRelativisticClocks(
        registry, frames, master, 1.0,
        std::span<const sim::ClockGravityBinding>(&missing, 1)).accepted);
    CHECK_EQ(registry.Get<ecs::RelativisticClock>(a).coordinateTime,
             beforeA.coordinateTime);
    CHECK_EQ(registry.Get<ecs::RelativisticClock>(a).properTimeDeviation,
             beforeA.properTimeDeviation);
    CHECK_EQ(registry.Get<ecs::RelativisticClock>(b).coordinateTime,
             beforeB.coordinateTime);
    CHECK_EQ(registry.Get<ecs::RelativisticClock>(b).properTimeDeviation,
             beforeB.properTimeDeviation);

    registry.Get<ecs::RelativisticClock>(b).coordinateTime =
        (std::numeric_limits<double>::quiet_NaN)();
    CHECK_FALSE(sim::StepRelativisticClocks(
        registry, frames, master, 1.0).accepted);
    CHECK_EQ(registry.Get<ecs::RelativisticClock>(a).coordinateTime,
             beforeA.coordinateTime);
}
