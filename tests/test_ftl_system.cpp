// =============================================================================
// tests/test_ftl_system.cpp - ECS/reference-frame FTL transition adapter
// =============================================================================

#include "test_framework.h"
#include "sim/ftl_system.h"

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

void CheckTransformExact(const ecs::Transform& a, const ecs::Transform& b)
{
    CHECK(a.position == b.position);
    CheckQuatExact(a.rotation, b.rotation);
    CheckVec3fExact(a.scale, b.scale);
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

void CheckOrbitExact(const ecs::OrbitState& a, const ecs::OrbitState& b)
{
    CHECK_EQ(a.elements.semiMajorAxis, b.elements.semiMajorAxis);
    CHECK_EQ(a.elements.eccentricity, b.elements.eccentricity);
    CHECK_EQ(a.elements.inclination, b.elements.inclination);
    CHECK_EQ(a.elements.longitudeAscNode, b.elements.longitudeAscNode);
    CHECK_EQ(a.elements.argPeriapsis, b.elements.argPeriapsis);
    CHECK_EQ(a.elements.trueAnomaly, b.elements.trueAnomaly);
    CHECK_EQ(a.primaryMu, b.primaryMu);
    CHECK_EQ(a.primaryBodyId, b.primaryBodyId);
    CHECK_EQ(a.epoch, b.epoch);
}

struct RequiredSnapshot
{
    ecs::Transform transform;
    ecs::SpatialFrame frame;
    ecs::RigidBody rigidBody;
};

RequiredSnapshot SnapshotRequired(const ecs::Registry& registry, ecs::Entity entity)
{
    return RequiredSnapshot{
        *registry.TryGet<ecs::Transform>(entity),
        *registry.TryGet<ecs::SpatialFrame>(entity),
        *registry.TryGet<ecs::RigidBody>(entity),
    };
}

void CheckRequiredExact(const ecs::Registry& registry, ecs::Entity entity,
                        const RequiredSnapshot& expected)
{
    CheckTransformExact(*registry.TryGet<ecs::Transform>(entity), expected.transform);
    CHECK_EQ(registry.TryGet<ecs::SpatialFrame>(entity)->frameId,
             expected.frame.frameId);
    CheckRigidBodyExact(*registry.TryGet<ecs::RigidBody>(entity), expected.rigidBody);
}

ecs::Entity MakeRequiredBody(ecs::Registry& registry, FrameId frame,
                             const Vec3d& position = Vec3d{ 2.0, 3.0, 4.0 })
{
    const ecs::Entity entity = registry.Create();

    ecs::Transform transform;
    transform.position = position;
    transform.rotation = Quatf::Identity();
    transform.scale = Vec3f{ 2.0f, 3.0f, 4.0f };
    registry.Assign<ecs::Transform>(entity, transform);

    registry.Assign<ecs::SpatialFrame>(
        entity, ecs::SpatialFrame{ static_cast<uint32_t>(frame) });

    ecs::RigidBody rigidBody;
    rigidBody.linearVelocity = Vec3d{ 4.0, -2.0, 8.0 };
    rigidBody.angularVelocity = Vec3f{ 0.25f, -0.5f, 0.75f };
    rigidBody.invMass = 0.125;
    rigidBody.invInertiaDiag = Vec3f{ 0.1f, 0.2f, 0.3f };
    rigidBody.forceAccum = Vec3d{ 10.0, 20.0, 30.0 };
    rigidBody.torqueAccum = Vec3f{ -4.0f, 5.0f, -6.0f };
    rigidBody.prevPosition = Vec3d{ 1.0, 2.0, 3.0 };
    rigidBody.prevRotation = Quatf::FromEuler(0.1f, -0.2f, 0.3f);
    registry.Assign<ecs::RigidBody>(entity, rigidBody);

    return entity;
}

} // namespace

TEST_CASE(FtlSystem_RotatedDistantTransition_ConvertsFramesAndCommitsRetainedState)
{
    FrameGraph frames;
    const WorldPos sourceOrigin{ 1200000, -3400000, 5600000,
                                 Vec3d{ 1000000.0, 2000000.0, 3000000.0 } };
    const WorldPos destinationOrigin{ -7200000, 8100000, -9100000,
                                      Vec3d{ 4000000.0, 5000000.0, 6000000.0 } };
    const Vec3d sourceFrameVelocity{ 100.0, -20.0, 5.0 };
    const Vec3d destinationFrameVelocity{ -50.0, 10.0, 7.0 };
    const FrameId sourceFrame = frames.CreateFrame(
        kInvalidFrame, sourceOrigin, sourceFrameVelocity);
    const FrameId destinationFrame = frames.CreateFrame(
        kInvalidFrame, destinationOrigin, destinationFrameVelocity);

    ecs::Registry registry;
    const Vec3d sourceLocal{ 2.0, 3.0, 4.0 };
    const ecs::Entity entity = MakeRequiredBody(registry, sourceFrame, sourceLocal);

    ecs::RelativisticBody relativistic;
    relativistic.momentum = Vec3d{ 1200.0, -300.0, 500.0 };
    relativistic.restMass = 42.0;
    registry.Assign<ecs::RelativisticBody>(entity, relativistic);

    ecs::GravitationalBody gravitational;
    gravitational.mu = 7.0;
    gravitational.radius = 8.0;
    gravitational.bodyId = 9;
    gravitational.isSource = false;
    gravitational.owner = ecs::OrbitOwner::OnRails;
    registry.Assign<ecs::GravitationalBody>(entity, gravitational);

    ecs::OrbitState orbit;
    orbit.elements.semiMajorAxis = 11.0;
    orbit.elements.eccentricity = 0.25;
    orbit.elements.inclination = 0.5;
    orbit.elements.longitudeAscNode = 0.75;
    orbit.elements.argPeriapsis = 1.0;
    orbit.elements.trueAnomaly = 1.25;
    orbit.primaryMu = 12.0;
    orbit.primaryBodyId = 13;
    orbit.epoch = 14.0;
    registry.Assign<ecs::OrbitState>(entity, orbit);

    const ecs::RigidBody beforeBody = *registry.TryGet<ecs::RigidBody>(entity);
    const Quatf rotation = Quatf::FromAxisAngle(Vec3f{ 0.0f, 1.0f, 0.0f },
                                                 1.57079632679f);
    const MouthTransform mouth = MouthTransform::Wormhole(
        sourceOrigin, destinationOrigin, rotation);

    const Vec3d expectedPosition = RotateVec3d(rotation, sourceLocal);
    const Vec3d expectedWorldVelocity = RotateVec3d(
        rotation, sourceFrameVelocity + beforeBody.linearVelocity);
    const Vec3d expectedLocalVelocity =
        expectedWorldVelocity - destinationFrameVelocity;
    const Vec3d expectedMomentum = RotateVec3d(rotation, relativistic.momentum);

    const FtlTransitionResult result = TryTeleportEntity(
        registry, entity, frames, destinationFrame, mouth);

    CHECK(result.accepted);
    CHECK(result.resetRenderHistory);
    CHECK(result.drainFixedAccumulator);

    const ecs::Transform& afterTransform = *registry.TryGet<ecs::Transform>(entity);
    const ecs::RigidBody& afterBody = *registry.TryGet<ecs::RigidBody>(entity);
    CHECK(Separation(
              Translate(destinationOrigin, expectedPosition),
              frames.ResolveWorldPos(Body{ destinationFrame,
                                           afterTransform.position,
                                           afterBody.linearVelocity })).Length() < 1e-6);
    CHECK((afterTransform.position - expectedPosition).Length() < 1e-6);
    CHECK((afterBody.linearVelocity - expectedLocalVelocity).Length() < 1e-9);
    CHECK_EQ(registry.TryGet<ecs::SpatialFrame>(entity)->frameId, destinationFrame);
    CHECK(afterBody.prevPosition == afterTransform.position);
    CheckQuatExact(afterBody.prevRotation, afterTransform.rotation);
    CheckVec3fExact(afterBody.angularVelocity, beforeBody.angularVelocity);
    CHECK(afterBody.forceAccum == Vec3d{});
    CheckVec3fExact(afterBody.torqueAccum, Vec3f{});
    CHECK((registry.TryGet<ecs::RelativisticBody>(entity)->momentum -
           expectedMomentum).Length() < 1e-9);
    CHECK_EQ(registry.TryGet<ecs::RelativisticBody>(entity)->restMass, 42.0);
    CHECK(registry.TryGet<ecs::GravitationalBody>(entity)->owner ==
          ecs::OrbitOwner::ForceIntegrated);
    CHECK_EQ(registry.TryGet<ecs::GravitationalBody>(entity)->mu, 7.0);
    CheckOrbitExact(*registry.TryGet<ecs::OrbitState>(entity), orbit);

    // Negative control: a frame transition cannot copy the old local position
    // or local velocity directly and still preserve the intended world state.
    CHECK((afterTransform.position - sourceLocal).Length() > 1.0);
    CHECK((afterBody.linearVelocity - beforeBody.linearVelocity).Length() > 1.0);
}

TEST_CASE(FtlSystem_MinimalRequiredComponents_SucceedWithoutOptionalState)
{
    FrameGraph frames;
    const WorldPos sourceOrigin = WorldPos::FromOffset(Vec3d{ 1000.0, 2000.0, 3000.0 });
    const WorldPos destinationOrigin = WorldPos::FromOffset(Vec3d{ 9000.0, 8000.0, 7000.0 });
    const FrameId sourceFrame = frames.CreateFrame(kInvalidFrame, sourceOrigin);
    const FrameId destinationFrame = frames.CreateFrame(kInvalidFrame, destinationOrigin);

    ecs::Registry registry;
    const ecs::Entity entity = MakeRequiredBody(registry, sourceFrame);
    const FtlTransitionResult result = TryTeleportEntity(
        registry, entity, frames, destinationFrame,
        MouthTransform::Wormhole(sourceOrigin, destinationOrigin));

    CHECK(result.accepted);
    CHECK(result.resetRenderHistory);
    CHECK(result.drainFixedAccumulator);
    CHECK(!registry.Has<ecs::RelativisticBody>(entity));
    CHECK(!registry.Has<ecs::GravitationalBody>(entity));
    CHECK_EQ(registry.TryGet<ecs::SpatialFrame>(entity)->frameId, destinationFrame);
    CHECK(registry.TryGet<ecs::RigidBody>(entity)->prevPosition ==
          registry.TryGet<ecs::Transform>(entity)->position);
}

TEST_CASE(FtlSystem_MissingStaleAndInvalidFrameRequests_AreNoOps)
{
    FrameGraph frames;
    const WorldPos origin = WorldPos::FromOffset(Vec3d{ 100.0, 200.0, 300.0 });
    const FrameId frame = frames.CreateFrame(kInvalidFrame, origin);
    const MouthTransform mouth = MouthTransform::Wormhole(origin, origin);

    ecs::Registry registry;
    const ecs::Entity missingFrame = registry.Create();
    ecs::Transform missingTransform;
    missingTransform.position = Vec3d{ 7.0, 8.0, 9.0 };
    registry.Assign<ecs::Transform>(missingFrame, missingTransform);
    ecs::RigidBody missingBody;
    missingBody.linearVelocity = Vec3d{ 1.0, 2.0, 3.0 };
    registry.Assign<ecs::RigidBody>(missingFrame, missingBody);

    const ecs::Transform missingTransformBefore =
        *registry.TryGet<ecs::Transform>(missingFrame);
    const ecs::RigidBody missingBodyBefore =
        *registry.TryGet<ecs::RigidBody>(missingFrame);
    const FtlTransitionResult missing = TryTeleportEntity(
        registry, missingFrame, frames, frame, mouth);
    CHECK(!missing.accepted);
    CHECK(!missing.resetRenderHistory);
    CHECK(!missing.drainFixedAccumulator);
    CheckTransformExact(*registry.TryGet<ecs::Transform>(missingFrame),
                        missingTransformBefore);
    CheckRigidBodyExact(*registry.TryGet<ecs::RigidBody>(missingFrame),
                        missingBodyBefore);

    const ecs::Entity invalidDestination = MakeRequiredBody(registry, frame);
    const RequiredSnapshot before = SnapshotRequired(registry, invalidDestination);
    const FtlTransitionResult invalid = TryTeleportEntity(
        registry, invalidDestination, frames, kInvalidFrame, mouth);
    CHECK(!invalid.accepted);
    CHECK(!invalid.resetRenderHistory);
    CHECK(!invalid.drainFixedAccumulator);
    CheckRequiredExact(registry, invalidDestination, before);

    const ecs::Entity stale = MakeRequiredBody(registry, frame);
    registry.Destroy(stale);
    const FtlTransitionResult staleResult = TryTeleportEntity(
        registry, stale, frames, frame, mouth);
    CHECK(!staleResult.accepted);
    CHECK(!staleResult.resetRenderHistory);
    CHECK(!staleResult.drainFixedAccumulator);
}

TEST_CASE(FtlSystem_InvalidTransformAndUnsafeDestination_AreAtomicNoOps)
{
    FrameGraph frames;
    const WorldPos sourceOrigin{ 10, 20, 30, Vec3d{ 1000.0, 2000.0, 3000.0 } };
    const WorldPos destinationOrigin{ 40, 50, 60,
                                      Vec3d{ 4000.0, 5000.0, 6000.0 } };
    const FrameId sourceFrame = frames.CreateFrame(kInvalidFrame, sourceOrigin);
    const FrameId destinationFrame = frames.CreateFrame(kInvalidFrame, destinationOrigin);

    ecs::Registry registry;
    const ecs::Entity entity = MakeRequiredBody(registry, sourceFrame);
    ecs::RelativisticBody relativistic;
    relativistic.momentum = Vec3d{ 1.0, 2.0, 3.0 };
    relativistic.restMass = 4.0;
    registry.Assign<ecs::RelativisticBody>(entity, relativistic);
    ecs::GravitationalBody gravitational;
    gravitational.owner = ecs::OrbitOwner::OnRails;
    registry.Assign<ecs::GravitationalBody>(entity, gravitational);

    const RequiredSnapshot requiredBefore = SnapshotRequired(registry, entity);
    const ecs::RelativisticBody relativisticBefore =
        *registry.TryGet<ecs::RelativisticBody>(entity);
    const ecs::GravitationalBody gravitationalBefore =
        *registry.TryGet<ecs::GravitationalBody>(entity);

    const float nan = (std::numeric_limits<float>::quiet_NaN)();
    const MouthTransform malformed = MouthTransform::Wormhole(
        sourceOrigin, destinationOrigin, Quatf{ nan, 0.0f, 0.0f, 1.0f });
    const FtlTransitionResult malformedResult = TryTeleportEntity(
        registry, entity, frames, destinationFrame, malformed);
    CHECK(!malformedResult.accepted);
    CHECK(!malformedResult.resetRenderHistory);
    CHECK(!malformedResult.drainFixedAccumulator);
    CheckRequiredExact(registry, entity, requiredBefore);
    CHECK(registry.TryGet<ecs::RelativisticBody>(entity)->momentum ==
          relativisticBefore.momentum);
    CHECK_EQ(registry.TryGet<ecs::RelativisticBody>(entity)->restMass,
             relativisticBefore.restMass);
    CHECK(registry.TryGet<ecs::GravitationalBody>(entity)->owner ==
          gravitationalBefore.owner);

    const WorldPos remoteExit{ destinationOrigin.sx + 2,
                               destinationOrigin.sy,
                               destinationOrigin.sz,
                               destinationOrigin.offset };
    const MouthTransform unsafe = MouthTransform::Wormhole(sourceOrigin, remoteExit);
    const FtlTransitionResult unsafeResult = TryTeleportEntity(
        registry, entity, frames, destinationFrame, unsafe);
    CHECK(!unsafeResult.accepted);
    CHECK(!unsafeResult.resetRenderHistory);
    CHECK(!unsafeResult.drainFixedAccumulator);
    CheckRequiredExact(registry, entity, requiredBefore);
    CHECK(registry.TryGet<ecs::RelativisticBody>(entity)->momentum ==
          relativisticBefore.momentum);
    CHECK(registry.TryGet<ecs::GravitationalBody>(entity)->owner ==
          gravitationalBefore.owner);

    registry.TryGet<ecs::GravitationalBody>(entity)->owner =
        static_cast<ecs::OrbitOwner>(99);
    const RequiredSnapshot invalidOwnerBefore = SnapshotRequired(registry, entity);
    const ecs::RelativisticBody invalidOwnerRelativisticBefore =
        *registry.TryGet<ecs::RelativisticBody>(entity);
    const FtlTransitionResult invalidOwnerResult = TryTeleportEntity(
        registry, entity, frames, destinationFrame,
        MouthTransform::Wormhole(sourceOrigin, destinationOrigin));
    CHECK(!invalidOwnerResult.accepted);
    CHECK(!invalidOwnerResult.resetRenderHistory);
    CHECK(!invalidOwnerResult.drainFixedAccumulator);
    CheckRequiredExact(registry, entity, invalidOwnerBefore);
    CHECK(registry.TryGet<ecs::RelativisticBody>(entity)->momentum ==
          invalidOwnerRelativisticBefore.momentum);
    CHECK(registry.TryGet<ecs::GravitationalBody>(entity)->owner ==
          static_cast<ecs::OrbitOwner>(99));
}
