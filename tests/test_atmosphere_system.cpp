// =============================================================================
// tests/test_atmosphere_system.cpp - ECS atmospheric-flight adapter
// =============================================================================

#include "test_framework.h"
#include "sim/atmosphere_system.h"
#include "sim/physics_system.h"
#include "sim/relativity.h"

#include <cmath>
#include <limits>

namespace
{

using core::Quatf;
using core::Vec3d;
using core::Vec3f;
using namespace sim;

void CheckVec3fExact(const Vec3f& a, const Vec3f& b)
{
    CHECK_EQ(a.x, b.x);
    CHECK_EQ(a.y, b.y);
    CHECK_EQ(a.z, b.z);
}

void CheckQuatExact(const Quatf& a, const Quatf& b)
{
    CHECK_EQ(a.x, b.x);
    CHECK_EQ(a.y, b.y);
    CHECK_EQ(a.z, b.z);
    CHECK_EQ(a.w, b.w);
}

void CheckRigidBodyExact(const ecs::RigidBody& a, const ecs::RigidBody& b)
{
    CHECK(a.linearVelocity == b.linearVelocity);
    CheckVec3fExact(a.angularVelocity, b.angularVelocity);
    CHECK_EQ(a.invMass, b.invMass);
    CheckVec3fExact(a.invInertiaDiag, b.invInertiaDiag);
    CHECK(a.forceAccum == b.forceAccum);
    CheckVec3fExact(a.torqueAccum, b.torqueAccum);
    CHECK(a.prevPosition == b.prevPosition);
    CheckQuatExact(a.prevRotation, b.prevRotation);
}

void CheckAeroExact(const ecs::AerodynamicBody& a,
                    const ecs::AerodynamicBody& b)
{
    CHECK_EQ(a.referenceArea, b.referenceArea);
    CHECK_EQ(a.baseDragCoefficient, b.baseDragCoefficient);
    CHECK_EQ(a.angleOfAttackDrag, b.angleOfAttackDrag);
    CHECK_EQ(a.liftSlope, b.liftSlope);
    CHECK_EQ(a.stallAngleRadians, b.stallAngleRadians);
    CHECK_EQ(a.noseRadius, b.noseRadius);
    CheckVec3fExact(a.liftAxis, b.liftAxis);
    CheckVec3fExact(a.centerOfPressure, b.centerOfPressure);
}

struct EntitySnapshot
{
    ecs::Transform transform;
    ecs::SpatialFrame frame;
    ecs::RigidBody rigidBody;
    ecs::AerodynamicBody aero;
    ecs::GravitationalBody gravity;
};

EntitySnapshot Snapshot(const ecs::Registry& registry, ecs::Entity entity)
{
    return EntitySnapshot{
        *registry.TryGet<ecs::Transform>(entity),
        *registry.TryGet<ecs::SpatialFrame>(entity),
        *registry.TryGet<ecs::RigidBody>(entity),
        *registry.TryGet<ecs::AerodynamicBody>(entity),
        *registry.TryGet<ecs::GravitationalBody>(entity),
    };
}

void CheckSnapshotExact(const ecs::Registry& registry, ecs::Entity entity,
                        const EntitySnapshot& expected)
{
    const ecs::Transform& transform = *registry.TryGet<ecs::Transform>(entity);
    CHECK(transform.position == expected.transform.position);
    CheckQuatExact(transform.rotation, expected.transform.rotation);
    CheckVec3fExact(transform.scale, expected.transform.scale);
    CHECK_EQ(registry.TryGet<ecs::SpatialFrame>(entity)->frameId,
             expected.frame.frameId);
    CheckRigidBodyExact(*registry.TryGet<ecs::RigidBody>(entity),
                        expected.rigidBody);
    CheckAeroExact(*registry.TryGet<ecs::AerodynamicBody>(entity), expected.aero);

    const ecs::GravitationalBody& gravity =
        *registry.TryGet<ecs::GravitationalBody>(entity);
    CHECK_EQ(gravity.mu, expected.gravity.mu);
    CHECK_EQ(gravity.radius, expected.gravity.radius);
    CHECK_EQ(gravity.bodyId, expected.gravity.bodyId);
    CHECK_EQ(gravity.isSource, expected.gravity.isSource);
    CHECK(gravity.owner == expected.gravity.owner);
}

ecs::Entity MakeBody(ecs::Registry& registry, FrameId frame,
                     const Vec3d& localPosition, const Vec3d& localVelocity,
                     const ecs::AerodynamicBody& aero = ecs::AerodynamicBody{})
{
    const ecs::Entity entity = registry.Create();

    ecs::Transform transform;
    transform.position = localPosition;
    transform.rotation = Quatf::Identity();
    transform.scale = Vec3f{ 1.5f, 2.0f, 2.5f };
    registry.Assign<ecs::Transform>(entity, transform);
    registry.Assign<ecs::SpatialFrame>(
        entity, ecs::SpatialFrame{ static_cast<uint32_t>(frame) });

    ecs::RigidBody rigidBody;
    rigidBody.linearVelocity = localVelocity;
    rigidBody.angularVelocity = Vec3f{ 0.1f, -0.2f, 0.3f };
    rigidBody.invMass = 0.1;
    rigidBody.invInertiaDiag = Vec3f{ 0.4f, 0.5f, 0.6f };
    rigidBody.forceAccum = Vec3d{ 3.0, 4.0, 5.0 };
    rigidBody.torqueAccum = Vec3f{ 0.7f, 0.8f, 0.9f };
    rigidBody.prevPosition = localPosition - Vec3d{ 1.0, 2.0, 3.0 };
    rigidBody.prevRotation = Quatf::FromEuler(0.1f, 0.2f, -0.3f);
    registry.Assign<ecs::RigidBody>(entity, rigidBody);
    registry.Assign<ecs::AerodynamicBody>(entity, aero);

    ecs::GravitationalBody gravity;
    gravity.mu = 6.0;
    gravity.radius = 7.0;
    gravity.bodyId = 8;
    gravity.isSource = false;
    gravity.owner = ecs::OrbitOwner::OnRails;
    registry.Assign<ecs::GravitationalBody>(entity, gravity);
    return entity;
}

AtmosphereEnvironment MakeEnvironment(const WorldPos& center,
                                      const Vec3d& linearVelocity = Vec3d{})
{
    AtmosphereEnvironment environment;
    environment.center = center;
    environment.linearVelocity = linearVelocity;
    environment.radius = 1000.0;
    environment.model = AtmosphereModel::ExponentialBody(1.2, 8000.0, 100000.0);
    return environment;
}

} // namespace

TEST_CASE(AtmosphereSystem_MovingFrameDrag_MatchesContractiveKernelExactly)
{
    const WorldPos center{ 123456, -234567, 345678,
                           Vec3d{ 5000000.0, 6000000.0, 7000000.0 } };
    const Vec3d frameVelocity{ 200.0, -50.0, 10.0 };
    FrameGraph frames;
    const FrameId frame = frames.CreateFrame(
        kInvalidFrame, center, frameVelocity);

    ecs::AerodynamicBody aero;
    aero.referenceArea = 2.0;
    aero.baseDragCoefficient = 1.0;
    aero.angleOfAttackDrag = 0.0;
    aero.liftSlope = 0.0;
    aero.centerOfPressure = Vec3f{};

    ecs::Registry registry;
    const Vec3d initialAirspeed{ 100.0, 0.0, 0.0 };
    const ecs::Entity entity = MakeBody(
        registry, frame, Vec3d{ 0.0, 1000.0, 0.0 }, initialAirspeed, aero);
    registry.TryGet<ecs::GravitationalBody>(entity)->owner =
        ecs::OrbitOwner::ForceIntegrated;
    const ecs::RigidBody before = *registry.TryGet<ecs::RigidBody>(entity);
    AtmosphereEnvironment environment = MakeEnvironment(center, frameVelocity);

    const double dt = 1.0;
    const AtmosphereState sampled = SampleAtmosphere(environment.model, 0.0);
    const double mach = MachNumber(initialAirspeed, sampled.speedOfSound);
    const double cd = DragCoefficientAtMach(aero.baseDragCoefficient, mach);
    const Vec3d expected = SemiImplicitDragAirspeed(
        initialAirspeed, sampled.density, cd, aero.referenceArea, 10.0, dt);
    const Vec3d explicitWrong = ExplicitDragAirspeed(
        initialAirspeed, sampled.density, cd, aero.referenceArea, 10.0, dt);

    const AtmosphereStepResult result = ApplyAtmosphereToEntity(
        registry, entity, frames, environment, dt);

    CHECK(result.accepted);
    CHECK(result.inAtmosphere);
    CHECK_APPROX_EPS(result.density, sampled.density, 1e-12);
    CHECK_APPROX_EPS(result.mach, mach, 1e-12);
    CHECK((registry.TryGet<ecs::RigidBody>(entity)->linearVelocity -
           expected).Length() < 1e-12);
    CHECK(registry.TryGet<ecs::RigidBody>(entity)->linearVelocity.Length() <
          initialAirspeed.Length());
    CHECK(explicitWrong.Length() > initialAirspeed.Length());
    CHECK(registry.TryGet<ecs::RigidBody>(entity)->forceAccum == before.forceAccum);
    CheckVec3fExact(registry.TryGet<ecs::RigidBody>(entity)->torqueAccum,
                    before.torqueAccum);
    CHECK(registry.TryGet<ecs::GravitationalBody>(entity)->owner ==
          ecs::OrbitOwner::ForceIntegrated);
}

TEST_CASE(AtmosphereSystem_CorotatingAir_HasZeroAirspeedAndNoWrench)
{
    const WorldPos center = WorldPos::FromOffset(
        Vec3d{ 1000000.0, 2000000.0, 3000000.0 });
    const Vec3d frameVelocity{ 20.0, 30.0, 40.0 };
    FrameGraph frames;
    const FrameId frame = frames.CreateFrame(
        kInvalidFrame, center, frameVelocity);

    AtmosphereEnvironment environment = MakeEnvironment(center, frameVelocity);
    environment.angularVelocity = Vec3d{ 0.0, 0.01, 0.0 };
    const Vec3d radial{ 1000.0, 0.0, 0.0 };
    const Vec3d corotation = environment.angularVelocity.Cross(radial);

    ecs::Registry registry;
    const ecs::Entity entity = MakeBody(registry, frame, radial, corotation);
    const ecs::RigidBody before = *registry.TryGet<ecs::RigidBody>(entity);

    const AtmosphereStepResult result = ApplyAtmosphereToEntity(
        registry, entity, frames, environment, 1.0 / 60.0);

    CHECK(result.accepted);
    CHECK(result.inAtmosphere);
    CHECK_EQ(result.dynamicPressure, 0.0);
    CHECK_EQ(result.mach, 0.0);
    CHECK_EQ(result.heatFlux, 0.0);
    ecs::RigidBody expected = before;
    expected.forceAccum = Vec3d{};
    expected.torqueAccum = Vec3f{};
    CheckRigidBodyExact(*registry.TryGet<ecs::RigidBody>(entity), expected);
    CHECK(registry.TryGet<ecs::GravitationalBody>(entity)->owner ==
          ecs::OrbitOwner::ForceIntegrated);
}

TEST_CASE(AtmosphereSystem_CeilingIsAcceptedExactNoOpAndKeepsRails)
{
    const WorldPos center = WorldPos::FromOffset(
        Vec3d{ 4000000.0, 5000000.0, 6000000.0 });
    FrameGraph frames;
    const FrameId frame = frames.CreateFrame(kInvalidFrame, center);
    AtmosphereEnvironment environment = MakeEnvironment(center);
    environment.model = AtmosphereModel::ExponentialBody(1.2, 100.0, 1000.0);

    ecs::Registry registry;
    const ecs::Entity entity = MakeBody(
        registry, frame, Vec3d{ 0.0, 2000.0, 0.0 }, Vec3d{ 100.0, 0.0, 0.0 });
    const EntitySnapshot before = Snapshot(registry, entity);

    const AtmosphereStepResult result = ApplyAtmosphereToEntity(
        registry, entity, frames, environment, 1.0 / 60.0);

    CHECK(result.accepted);
    CHECK(!result.inAtmosphere);
    CHECK_EQ(result.density, 0.0);
    CHECK_EQ(result.dynamicPressure, 0.0);
    CheckSnapshotExact(registry, entity, before);

    environment.model = AtmosphereModel::Vacuum();
    const AtmosphereStepResult vacuum = ApplyAtmosphereToEntity(
        registry, entity, frames, environment, 1.0 / 60.0);
    CHECK(vacuum.accepted);
    CHECK(!vacuum.inAtmosphere);
    CHECK_EQ(vacuum.density, 0.0);
    CheckSnapshotExact(registry, entity, before);
}

TEST_CASE(AtmosphereSystem_GravitationalBodyIsOptional)
{
    const WorldPos center = WorldPos::FromOffset(
        Vec3d{ 4500000.0, 5500000.0, 6500000.0 });
    FrameGraph frames;
    const FrameId frame = frames.CreateFrame(kInvalidFrame, center);
    const AtmosphereEnvironment environment = MakeEnvironment(center);

    ecs::Registry registry;
    const ecs::Entity entity = MakeBody(
        registry, frame, Vec3d{ 0.0, 1000.0, 0.0 }, Vec3d{ 75.0, 0.0, 0.0 });
    registry.Remove<ecs::GravitationalBody>(entity);
    const Vec3d beforeVelocity =
        registry.TryGet<ecs::RigidBody>(entity)->linearVelocity;

    const AtmosphereStepResult result = ApplyAtmosphereToEntity(
        registry, entity, frames, environment, 1.0 / 60.0);

    CHECK(result.accepted);
    CHECK(result.inAtmosphere);
    CHECK(registry.TryGet<ecs::GravitationalBody>(entity) == nullptr);
    CHECK(registry.TryGet<ecs::RigidBody>(entity)->linearVelocity.Length() <
          beforeVelocity.Length());
}

TEST_CASE(AtmosphereSystem_LiftIsPerpendicularAndCopOrderingSetsStability)
{
    const WorldPos center = WorldPos::FromOffset(
        Vec3d{ 7000000.0, 8000000.0, 9000000.0 });
    FrameGraph frames;
    const FrameId frame = frames.CreateFrame(kInvalidFrame, center);
    const Vec3d airspeed{ 0.0, -10.0, 100.0 };

    ecs::AerodynamicBody aftAero;
    aftAero.referenceArea = 1.0;
    aftAero.baseDragCoefficient = 0.1;
    aftAero.angleOfAttackDrag = 0.0;
    aftAero.liftSlope = 4.0;
    aftAero.stallAngleRadians = 0.4;
    aftAero.centerOfPressure = Vec3f{ 0.0f, 0.0f, -2.0f };

    ecs::Registry aftRegistry;
    const ecs::Entity aft = MakeBody(
        aftRegistry, frame, Vec3d{ 0.0, 1000.0, 0.0 }, airspeed, aftAero);
    aftRegistry.TryGet<ecs::RigidBody>(aft)->forceAccum = Vec3d{};
    aftRegistry.TryGet<ecs::RigidBody>(aft)->torqueAccum = Vec3f{};

    AtmosphereEnvironment environment = MakeEnvironment(center);
    const AtmosphereStepResult aftResult = ApplyAtmosphereToEntity(
        aftRegistry, aft, frames, environment, 0.01);
    const ecs::RigidBody& aftBody = *aftRegistry.TryGet<ecs::RigidBody>(aft);

    CHECK(aftResult.accepted);
    CHECK(aftResult.inAtmosphere);
    CHECK(aftResult.angleOfAttack > 0.0);
    CHECK(aftBody.forceAccum.Length() > 0.0);
    CHECK(std::fabs(aftBody.forceAccum.Dot(airspeed)) < 1e-8);
    CHECK(aftBody.torqueAccum.x > 0.0f);

    ecs::AerodynamicBody forwardAero = aftAero;
    forwardAero.centerOfPressure.z = 2.0f;
    ecs::Registry forwardRegistry;
    const ecs::Entity forward = MakeBody(
        forwardRegistry, frame, Vec3d{ 0.0, 1000.0, 0.0 }, airspeed, forwardAero);
    forwardRegistry.TryGet<ecs::RigidBody>(forward)->forceAccum = Vec3d{};
    forwardRegistry.TryGet<ecs::RigidBody>(forward)->torqueAccum = Vec3f{};
    const AtmosphereStepResult forwardResult = ApplyAtmosphereToEntity(
        forwardRegistry, forward, frames, environment, 0.01);

    CHECK(forwardResult.accepted);
    CHECK(forwardRegistry.TryGet<ecs::RigidBody>(forward)->torqueAccum.x < 0.0f);
}

TEST_CASE(AtmosphereSystem_InvalidRequestsAreAtomicNoOps)
{
    const WorldPos center = WorldPos::FromOffset(
        Vec3d{ 10000.0, 20000.0, 30000.0 });
    FrameGraph frames;
    const FrameId frame = frames.CreateFrame(kInvalidFrame, center);
    AtmosphereEnvironment environment = MakeEnvironment(center);

    ecs::Registry registry;
    const ecs::Entity entity = MakeBody(
        registry, frame, Vec3d{ 0.0, 1000.0, 0.0 }, Vec3d{ 50.0, 0.0, 0.0 });
    const EntitySnapshot before = Snapshot(registry, entity);

    const AtmosphereStepResult badDt = ApplyAtmosphereToEntity(
        registry, entity, frames, environment,
        (std::numeric_limits<double>::quiet_NaN)());
    CHECK(!badDt.accepted);
    CheckSnapshotExact(registry, entity, before);

    AtmosphereEnvironment badEnvironment = environment;
    badEnvironment.angularVelocity.x =
        (std::numeric_limits<double>::infinity)();
    const AtmosphereStepResult badOmega = ApplyAtmosphereToEntity(
        registry, entity, frames, badEnvironment, 0.1);
    CHECK(!badOmega.accepted);
    CheckSnapshotExact(registry, entity, before);

    badEnvironment = environment;
    badEnvironment.model.kind = static_cast<AtmosphereKind>(99);
    const AtmosphereStepResult badModel = ApplyAtmosphereToEntity(
        registry, entity, frames, badEnvironment, 0.1);
    CHECK(!badModel.accepted);
    CheckSnapshotExact(registry, entity, before);

    registry.TryGet<ecs::SpatialFrame>(entity)->frameId = kInvalidFrame;
    const EntitySnapshot badFrameBefore = Snapshot(registry, entity);
    const AtmosphereStepResult badFrame = ApplyAtmosphereToEntity(
        registry, entity, frames, environment, 0.1);
    CHECK(!badFrame.accepted);
    CheckSnapshotExact(registry, entity, badFrameBefore);
    registry.TryGet<ecs::SpatialFrame>(entity)->frameId = frame;

    registry.TryGet<ecs::RigidBody>(entity)->invMass = 0.0;
    const EntitySnapshot zeroMassBefore = Snapshot(registry, entity);
    const AtmosphereStepResult zeroMass = ApplyAtmosphereToEntity(
        registry, entity, frames, environment, 0.1);
    CHECK(!zeroMass.accepted);
    CheckSnapshotExact(registry, entity, zeroMassBefore);

    const ecs::Entity missing = registry.Create();
    registry.Assign<ecs::Transform>(missing, ecs::Transform{});
    CHECK(!ApplyAtmosphereToEntity(
        registry, missing, frames, environment, 0.1).accepted);
    registry.Destroy(missing);
    CHECK(!ApplyAtmosphereToEntity(
        registry, missing, frames, environment, 0.1).accepted);
}

TEST_CASE(AtmosphereSystem_DeterministicReplay_IsBitExact)
{
    const WorldPos center{ -98765, 87654, -76543,
                           Vec3d{ 3500000.0, 4500000.0, 5500000.0 } };
    const Vec3d frameVelocity{ -12.0, 34.0, -56.0 };
    FrameGraph frames;
    const FrameId frame = frames.CreateFrame(
        kInvalidFrame, center, frameVelocity);
    AtmosphereEnvironment environment = MakeEnvironment(center, frameVelocity);

    ecs::AerodynamicBody aero;
    aero.referenceArea = 1.75;
    aero.baseDragCoefficient = 0.4;
    aero.angleOfAttackDrag = 0.0;
    aero.liftSlope = 0.0;
    aero.centerOfPressure = Vec3f{};

    ecs::Registry a;
    ecs::Registry b;
    const ecs::Entity ea = MakeBody(
        a, frame, Vec3d{ 0.0, 1200.0, 0.0 }, Vec3d{ 80.0, -5.0, 2.0 }, aero);
    const ecs::Entity eb = MakeBody(
        b, frame, Vec3d{ 0.0, 1200.0, 0.0 }, Vec3d{ 80.0, -5.0, 2.0 }, aero);

    AtmosphereStepResult ra;
    AtmosphereStepResult rb;
    for (int i = 0; i < 120; ++i)
    {
        ra = ApplyAtmosphereToEntity(a, ea, frames, environment, 1.0 / 60.0);
        rb = ApplyAtmosphereToEntity(b, eb, frames, environment, 1.0 / 60.0);
    }

    CHECK(ra.accepted && rb.accepted);
    CHECK_EQ(ra.inAtmosphere, rb.inAtmosphere);
    CHECK_EQ(ra.density, rb.density);
    CHECK_EQ(ra.dynamicPressure, rb.dynamicPressure);
    CHECK_EQ(ra.mach, rb.mach);
    CHECK_EQ(ra.angleOfAttack, rb.angleOfAttack);
    CHECK_EQ(ra.heatFlux, rb.heatFlux);
    CheckRigidBodyExact(*a.TryGet<ecs::RigidBody>(ea),
                        *b.TryGet<ecs::RigidBody>(eb));
    CHECK(a.TryGet<ecs::GravitationalBody>(ea)->owner ==
          b.TryGet<ecs::GravitationalBody>(eb)->owner);
}

TEST_CASE(AtmosphereSystem_RelativisticMomentumKeepsDragAcrossFlightStep)
{
    const WorldPos center = WorldPos::FromOffset(
        Vec3d{ 20000.0, 30000.0, 40000.0 });
    FrameGraph frames;
    const FrameId frame = frames.CreateFrame(kInvalidFrame, center);

    ecs::AerodynamicBody aero;
    aero.referenceArea = 3.0;
    aero.baseDragCoefficient = 1.0;
    aero.angleOfAttackDrag = 0.0;
    aero.liftSlope = 0.0;
    aero.centerOfPressure = Vec3f{};

    ecs::Registry registry;
    const Vec3d initialVelocity{ 250.0, 0.0, 0.0 };
    const ecs::Entity entity = MakeBody(
        registry, frame, Vec3d{ 0.0, 1000.0, 0.0 }, initialVelocity, aero);
    ecs::RigidBody* body = registry.TryGet<ecs::RigidBody>(entity);
    body->forceAccum = Vec3d{};
    body->torqueAccum = Vec3f{};
    registry.TryGet<ecs::GravitationalBody>(entity)->owner =
        ecs::OrbitOwner::ForceIntegrated;

    ecs::RelativisticBody relativistic;
    relativistic.restMass = 1.0 / body->invMass;
    relativistic.momentum = MomentumFromVelocity(
        initialVelocity, relativistic.restMass);
    registry.Assign<ecs::RelativisticBody>(entity, relativistic);

    const AtmosphereEnvironment environment = MakeEnvironment(center);
    constexpr double dt = 0.5;
    const AtmosphereStepResult atmosphere = ApplyAtmosphereToEntity(
        registry, entity, frames, environment, dt);
    CHECK(atmosphere.accepted);
    CHECK(atmosphere.inAtmosphere);

    const Vec3d draggedVelocity = body->linearVelocity;
    CHECK(draggedVelocity.Length() < initialVelocity.Length());
    CHECK(registry.Get<ecs::RelativisticBody>(entity).momentum ==
          MomentumFromVelocity(draggedVelocity, relativistic.restMass));

    const FlightPhysicsStepResult flight = StepFlightPhysics(registry, dt);
    CHECK(flight.accepted);
    CHECK_EQ(flight.relativisticBodyCount, 1u);
    CHECK(registry.Get<ecs::RigidBody>(entity).linearVelocity ==
          draggedVelocity);
    const Vec3d expectedPosition =
        Vec3d{ 0.0, 1000.0, 0.0 } + draggedVelocity * dt;
    CHECK(registry.Get<ecs::Transform>(entity).position == expectedPosition);
}
