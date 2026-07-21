// =============================================================================
// tests/test_simulation_system.cpp - fixed-step phase-order integration
// =============================================================================

#include "test_framework.h"
#include "sim/simulation_system.h"

namespace
{

ecs::Entity MakeSimulationBody(ecs::Registry& registry, sim::FrameId frame,
                               uint64_t bodyId, ecs::OrbitOwner owner,
                               const core::Vec3d& position,
                               const core::Vec3d& velocity,
                               double mu, bool isSource)
{
    const ecs::Entity entity = registry.Create();
    ecs::Transform transform;
    transform.position = position;
    registry.Assign<ecs::Transform>(entity, transform);
    ecs::RigidBody body;
    body.linearVelocity = velocity;
    body.invMass = 1.0;
    body.invInertiaDiag = core::Vec3f{ 1.0f, 1.0f, 1.0f };
    registry.Assign<ecs::RigidBody>(entity, body);
    registry.Assign<ecs::SpatialFrame>(
        entity, ecs::SpatialFrame{ static_cast<uint32_t>(frame) });
    ecs::GravitationalBody gravity;
    gravity.bodyId = bodyId;
    gravity.owner = owner;
    gravity.mu = mu;
    gravity.radius = isSource ? 1.0 : 0.0;
    gravity.isSource = isSource;
    registry.Assign<ecs::GravitationalBody>(entity, gravity);
    return entity;
}

} // namespace

TEST_CASE(SimulationSystem_ComposesPassiveGravityFlightAndClock)
{
    sim::FrameGraph frames;
    const sim::FrameId root = frames.CreateFrame(
        sim::kInvalidFrame, sim::WorldPos{});
    ecs::Registry registry;
    MakeSimulationBody(
        registry, root, 10, ecs::OrbitOwner::NBodyActive,
        core::Vec3d{}, core::Vec3d{}, 1.0e6, true);
    const ecs::Entity ship = MakeSimulationBody(
        registry, root, 20, ecs::OrbitOwner::ForceIntegrated,
        core::Vec3d{ 100.0, 0.0, 0.0 }, core::Vec3d{}, 0.0, false);
    registry.Assign<ecs::RelativisticClock>(
        ship, ecs::RelativisticClock{});

    const sim::ClockGravityBinding clockBinding{ ship, 10 };
    sim::SimulationStepConfig config;
    config.activeFrame = root;
    config.masterFrame = root;
    config.coordinateTime = 50.0;
    config.clockGravityBindings =
        std::span<const sim::ClockGravityBinding>(&clockBinding, 1);

    constexpr double dt = 0.01;
    const sim::SimulationStepResult result =
        sim::StepSimulation(registry, frames, dt, config);
    CHECK(result.accepted);
    CHECK(result.completedStage == sim::SimulationStepStage::Complete);
    CHECK_EQ(result.passiveOrbit.nbodyBodyCount, 1u);
    CHECK_EQ(result.gravity.sourceCount, 1u);
    CHECK_EQ(result.gravity.targetCount, 1u);
    CHECK_EQ(result.flight.advancedBodyCount, 1u);
    CHECK_EQ(result.clocks.clockCount, 1u);
    CHECK_EQ(result.clocks.gravitationalClockCount, 1u);
    CHECK(registry.Get<ecs::Transform>(ship).position.x < 100.0);
    CHECK(registry.Get<ecs::RigidBody>(ship).forceAccum == core::Vec3d{});
    CHECK_EQ(registry.Get<ecs::RelativisticClock>(ship).coordinateTime, dt);
}

TEST_CASE(SimulationSystem_AtmospherePromotesBeforeOwnerDispatch)
{
    sim::FrameGraph frames;
    const sim::WorldPos center = sim::WorldPos::FromOffset(
        core::Vec3d{ 1000.0, 2000.0, 3000.0 });
    const sim::FrameId root = frames.CreateFrame(sim::kInvalidFrame, center);
    ecs::Registry registry;
    const ecs::Entity ship = MakeSimulationBody(
        registry, root, 30, ecs::OrbitOwner::OnRails,
        core::Vec3d{ 0.0, 1000.0, 0.0 },
        core::Vec3d{ 100.0, 0.0, 0.0 }, 0.0, false);
    registry.Assign<ecs::AerodynamicBody>(ship, ecs::AerodynamicBody{});
    ecs::OrbitState orbit;
    orbit.elements.semiMajorAxis = 1000.0;
    orbit.elements.eccentricity = 0.0;
    orbit.primaryMu = 1.0e6;
    orbit.primaryBodyId = 999;
    registry.Assign<ecs::OrbitState>(ship, orbit);

    sim::AtmosphereEnvironment environment;
    environment.center = center;
    environment.radius = 1000.0;
    environment.model = sim::AtmosphereModel::ExponentialBody(
        1.2, 8000.0, 100000.0);
    const sim::AtmosphereBinding atmosphere{ ship, environment };
    sim::SimulationStepConfig config;
    config.activeFrame = root;
    config.masterFrame = root;
    config.atmosphereBindings =
        std::span<const sim::AtmosphereBinding>(&atmosphere, 1);

    const core::Vec3d before = registry.Get<ecs::Transform>(ship).position;
    const sim::SimulationStepResult result =
        sim::StepSimulation(registry, frames, 0.1, config);
    CHECK(result.accepted);
    CHECK(result.atmosphere[0].inAtmosphere);
    CHECK(registry.Get<ecs::GravitationalBody>(ship).owner ==
          ecs::OrbitOwner::ForceIntegrated);
    CHECK_EQ(result.passiveOrbit.railBodyCount, 0u);
    CHECK_EQ(result.flight.advancedBodyCount, 1u);
    CHECK(registry.Get<ecs::Transform>(ship).position != before);
}

TEST_CASE(SimulationSystem_DuplicateOperatorsRejectBeforeWrite)
{
    sim::FrameGraph frames;
    const sim::FrameId root = frames.CreateFrame(
        sim::kInvalidFrame, sim::WorldPos{});
    ecs::Registry registry;
    const ecs::Entity ship = MakeSimulationBody(
        registry, root, 40, ecs::OrbitOwner::ForceIntegrated,
        core::Vec3d{ 1.0, 2.0, 3.0 }, core::Vec3d{}, 0.0, false);
    registry.Assign<ecs::AerodynamicBody>(ship, ecs::AerodynamicBody{});
    sim::AtmosphereEnvironment environment;
    environment.center = sim::WorldPos{};
    environment.radius = 1.0;
    const sim::AtmosphereBinding duplicate[2] = {
        { ship, environment }, { ship, environment },
    };
    sim::SimulationStepConfig config;
    config.activeFrame = root;
    config.masterFrame = root;
    config.atmosphereBindings = duplicate;
    const core::Vec3d before = registry.Get<ecs::Transform>(ship).position;

    const sim::SimulationStepResult result =
        sim::StepSimulation(registry, frames, 0.1, config);
    CHECK_FALSE(result.accepted);
    CHECK(result.completedStage == sim::SimulationStepStage::None);
    CHECK(registry.Get<ecs::Transform>(ship).position == before);
}
