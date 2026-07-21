// =============================================================================
// sim/snapshot_system.cpp - topology-preserving ECS snapshot transaction
// =============================================================================

#include "snapshot_system.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sim
{

namespace
{

bool IsZero(const core::Vec3d& value)
{
    return value.x == 0.0 && value.y == 0.0 && value.z == 0.0;
}

bool IsZero(const core::Vec3f& value)
{
    return value.x == 0.0f && value.y == 0.0f && value.z == 0.0f;
}

SnapshotBuildResult BuildFailure(const char* message)
{
    SnapshotBuildResult result;
    result.error = message;
    return result;
}

SnapshotApplyResult ApplyFailure(const char* message)
{
    SnapshotApplyResult result;
    result.error = message;
    return result;
}

struct CurrentBody
{
    ecs::Entity entity = ecs::NullEntity;
    const BodyRecord* body = nullptr;
    const GravRecord* gravity = nullptr;
    const ClockRecord* clock = nullptr;
};

} // namespace

SnapshotBuildResult BuildSnapshot(const ecs::Registry& registry,
                                  const FrameGraph& frames,
                                  double coordinateTime,
                                  double fixedDt,
                                  uint64_t simTick,
                                  FrameId masterFrame)
{
    if (!std::isfinite(coordinateTime) || !(fixedDt > 0.0) ||
        !std::isfinite(fixedDt))
        return BuildFailure("invalid simulation globals");
    if (masterFrame != kInvalidFrame && masterFrame >= frames.FrameCount())
        return BuildFailure("master frame is out of range");

    SimSnapshot snapshot;
    snapshot.coordinateTimeEpoch = coordinateTime;
    snapshot.fixedDt = fixedDt;
    snapshot.simTick = simTick;
    snapshot.masterFrame = masterFrame;
    snapshot.frames = frames.Frames();

    const auto* gravityPool = registry.GetPool<ecs::GravitationalBody>();
    const auto* rigidPool = registry.GetPool<ecs::RigidBody>();
    const auto* spatialPool = registry.GetPool<ecs::SpatialFrame>();
    const auto* orbitPool = registry.GetPool<ecs::OrbitState>();
    const auto* relativisticPool = registry.GetPool<ecs::RelativisticBody>();
    const auto* clockPool = registry.GetPool<ecs::RelativisticClock>();
    const uint32_t gravityCount = gravityPool ? gravityPool->Count() : 0u;
    if ((rigidPool ? rigidPool->Count() : 0u) != gravityCount ||
        (spatialPool ? spatialPool->Count() : 0u) != gravityCount)
        return BuildFailure("simulation bodies do not share one complete topology");
    if ((relativisticPool ? relativisticPool->Count() : 0u) !=
        (clockPool ? clockPool->Count() : 0u))
        return BuildFailure("relativistic body and clock topology differ");

    snapshot.bodies.reserve(gravityCount);
    snapshot.gravity.reserve(gravityCount);
    uint32_t orbitCount = 0;
    for (uint32_t i = 0; i < gravityCount; ++i)
    {
        const uint32_t entityIndex = gravityPool->EntityAt(i);
        const ecs::Entity entity = registry.EntityAtIndex(entityIndex);
        if (entity.IsNull() || !registry.Has<ecs::Transform>(entity) ||
            !registry.Has<ecs::RigidBody>(entity) ||
            !registry.Has<ecs::SpatialFrame>(entity))
            return BuildFailure("simulation body is missing a required component");

        const ecs::Transform& transform = registry.Get<ecs::Transform>(entity);
        const ecs::RigidBody& rigid = registry.Get<ecs::RigidBody>(entity);
        const ecs::SpatialFrame& spatial = registry.Get<ecs::SpatialFrame>(entity);
        const ecs::GravitationalBody& gravity = gravityPool->DataAt(i);
        if (!IsZero(rigid.forceAccum) || !IsZero(rigid.torqueAccum))
            return BuildFailure("snapshot requested with a pending wrench");

        BodyRecord bodyRecord;
        bodyRecord.bodyId = gravity.bodyId;
        bodyRecord.frame = static_cast<FrameId>(spatial.frameId);
        bodyRecord.position = transform.position;
        bodyRecord.rotation = transform.rotation;
        bodyRecord.scale = transform.scale;
        bodyRecord.velocityFrame = bodyRecord.frame;
        bodyRecord.linearVelocity = rigid.linearVelocity;
        bodyRecord.angularVelocity = rigid.angularVelocity;
        bodyRecord.invMass = rigid.invMass;
        bodyRecord.invInertiaDiag = rigid.invInertiaDiag;
        snapshot.bodies.push_back(bodyRecord);

        GravRecord gravRecord;
        gravRecord.bodyId = gravity.bodyId;
        gravRecord.mu = gravity.mu;
        gravRecord.radius = gravity.radius;
        gravRecord.isSource = gravity.isSource ? 1u : 0u;
        gravRecord.owner = static_cast<uint8_t>(gravity.owner);
        if (registry.Has<ecs::OrbitState>(entity))
        {
            ++orbitCount;
            const ecs::OrbitState& orbit = registry.Get<ecs::OrbitState>(entity);
            gravRecord.hasRails = 1u;
            gravRecord.semiMajorAxis = orbit.elements.semiMajorAxis;
            gravRecord.eccentricity = orbit.elements.eccentricity;
            gravRecord.inclination = orbit.elements.inclination;
            gravRecord.longitudeAscNode = orbit.elements.longitudeAscNode;
            gravRecord.argPeriapsis = orbit.elements.argPeriapsis;
            gravRecord.trueAnomaly = orbit.elements.trueAnomaly;
            gravRecord.primaryMu = orbit.primaryMu;
            gravRecord.primaryBodyId = orbit.primaryBodyId;
            gravRecord.railsEpoch = orbit.epoch;
        }
        snapshot.gravity.push_back(gravRecord);

        const bool hasRelativistic = registry.Has<ecs::RelativisticBody>(entity);
        const bool hasClock = registry.Has<ecs::RelativisticClock>(entity);
        if (hasRelativistic != hasClock)
            return BuildFailure("relativistic body and clock topology differ");
        if (hasRelativistic)
        {
            const ecs::RelativisticBody& relativistic =
                registry.Get<ecs::RelativisticBody>(entity);
            const ecs::RelativisticClock& clock =
                registry.Get<ecs::RelativisticClock>(entity);
            snapshot.clocks.push_back(ClockRecord{
                gravity.bodyId, relativistic.momentum, relativistic.restMass,
                clock.coordinateTime, clock.properTimeDeviation,
            });
        }
    }
    if (orbitCount != (orbitPool ? orbitPool->Count() : 0u) ||
        snapshot.clocks.size() !=
            (relativisticPool ? relativisticPool->Count() : 0u))
        return BuildFailure("simulation sidecar exists outside body topology");

    // Reuse the shipped codec's authoritative semantic validator and canonical
    // ordering. This also proves the bridge emits a representation the real load
    // path accepts, rather than maintaining a second validator that can drift.
    SimLoadResult validated = Deserialize(Serialize(snapshot));
    if (!validated.Ok())
    {
        SnapshotBuildResult failure;
        failure.error = "snapshot validation failed: " + validated.error;
        return failure;
    }

    SnapshotBuildResult result;
    result.accepted = true;
    result.snapshot = std::move(validated.snapshot);
    return result;
}

SnapshotApplyResult ApplySnapshot(ecs::Registry& registry,
                                  FrameGraph& frames,
                                  const SimSnapshot& input)
{
    SimLoadResult validated = Deserialize(Serialize(input));
    if (!validated.Ok())
    {
        SnapshotApplyResult failure;
        failure.error = "snapshot validation failed: " + validated.error;
        return failure;
    }
    const SimSnapshot& snapshot = validated.snapshot;
    if (snapshot.bodies.size() != snapshot.gravity.size())
        return ApplyFailure("snapshot topology requires one gravity record per body");

    std::unordered_map<uint64_t, const BodyRecord*> bodies;
    std::unordered_map<uint64_t, const GravRecord*> gravity;
    std::unordered_map<uint64_t, const ClockRecord*> clocks;
    bodies.reserve(snapshot.bodies.size() * 2u);
    gravity.reserve(snapshot.gravity.size() * 2u);
    clocks.reserve(snapshot.clocks.size() * 2u);
    for (const BodyRecord& record : snapshot.bodies)
    {
        if (record.frame != record.velocityFrame)
            return ApplyFailure("ECS supports one shared pose and velocity frame");
        bodies.emplace(record.bodyId, &record);
    }
    for (const GravRecord& record : snapshot.gravity)
        gravity.emplace(record.bodyId, &record);
    for (const ClockRecord& record : snapshot.clocks)
        clocks.emplace(record.bodyId, &record);

    const auto* gravityPool = registry.GetPool<ecs::GravitationalBody>();
    const auto* rigidPool = registry.GetPool<ecs::RigidBody>();
    const auto* spatialPool = registry.GetPool<ecs::SpatialFrame>();
    const auto* orbitPool = registry.GetPool<ecs::OrbitState>();
    const auto* relativisticPool = registry.GetPool<ecs::RelativisticBody>();
    const auto* clockPool = registry.GetPool<ecs::RelativisticClock>();
    const uint32_t currentCount = gravityPool ? gravityPool->Count() : 0u;
    if (currentCount != snapshot.gravity.size() ||
        (rigidPool ? rigidPool->Count() : 0u) != currentCount ||
        (spatialPool ? spatialPool->Count() : 0u) != currentCount)
        return ApplyFailure("live simulation topology differs from snapshot");
    const uint32_t snapshotOrbitCount = static_cast<uint32_t>(std::count_if(
        snapshot.gravity.begin(), snapshot.gravity.end(),
        [](const GravRecord& record) { return record.hasRails != 0u; }));
    if ((orbitPool ? orbitPool->Count() : 0u) != snapshotOrbitCount ||
        (relativisticPool ? relativisticPool->Count() : 0u) != snapshot.clocks.size() ||
        (clockPool ? clockPool->Count() : 0u) != snapshot.clocks.size())
        return ApplyFailure("live simulation sidecar topology differs from snapshot");

    std::vector<CurrentBody> staged;
    staged.reserve(currentCount);
    for (uint32_t i = 0; i < currentCount; ++i)
    {
        const uint32_t entityIndex = gravityPool->EntityAt(i);
        const ecs::Entity entity = registry.EntityAtIndex(entityIndex);
        if (entity.IsNull() || !registry.Has<ecs::Transform>(entity) ||
            !registry.Has<ecs::RigidBody>(entity) ||
            !registry.Has<ecs::SpatialFrame>(entity))
            return ApplyFailure("live simulation body is incomplete");

        const uint64_t bodyId = gravityPool->DataAt(i).bodyId;
        const auto bodyIt = bodies.find(bodyId);
        const auto gravityIt = gravity.find(bodyId);
        if (bodyIt == bodies.end() || gravityIt == gravity.end())
            return ApplyFailure("stable body IDs differ from snapshot");

        const bool hasOrbit = registry.Has<ecs::OrbitState>(entity);
        if (hasOrbit != (gravityIt->second->hasRails != 0u))
            return ApplyFailure("orbit sidecar topology differs from snapshot");
        const auto clockIt = clocks.find(bodyId);
        const bool snapshotHasClock = clockIt != clocks.end();
        const bool hasRelativistic = registry.Has<ecs::RelativisticBody>(entity);
        const bool hasClock = registry.Has<ecs::RelativisticClock>(entity);
        if (hasRelativistic != snapshotHasClock || hasClock != snapshotHasClock)
            return ApplyFailure("relativistic sidecar topology differs from snapshot");

        staged.push_back(CurrentBody{
            entity, bodyIt->second, gravityIt->second,
            snapshotHasClock ? clockIt->second : nullptr,
        });
    }
    if (staged.size() != bodies.size())
        return ApplyFailure("snapshot contains an unmapped simulation body");

    // Allocate the replacement frame vector before the first ECS write. The
    // commit below is POD field assignment plus a vector move and cannot fail.
    std::vector<Frame> replacementFrames = snapshot.frames;
    for (const CurrentBody& next : staged)
    {
        ecs::Transform& transform = registry.Get<ecs::Transform>(next.entity);
        ecs::RigidBody& rigid = registry.Get<ecs::RigidBody>(next.entity);
        ecs::SpatialFrame& spatial = registry.Get<ecs::SpatialFrame>(next.entity);
        ecs::GravitationalBody& grav =
            registry.Get<ecs::GravitationalBody>(next.entity);

        transform.position = next.body->position;
        transform.rotation = next.body->rotation;
        transform.scale = next.body->scale;
        rigid.linearVelocity = next.body->linearVelocity;
        rigid.angularVelocity = next.body->angularVelocity;
        rigid.invMass = next.body->invMass;
        rigid.invInertiaDiag = next.body->invInertiaDiag;
        rigid.forceAccum = {};
        rigid.torqueAccum = {};
        rigid.prevPosition = transform.position;
        rigid.prevRotation = transform.rotation;
        spatial.frameId = next.body->frame;
        grav.mu = next.gravity->mu;
        grav.radius = next.gravity->radius;
        grav.bodyId = next.gravity->bodyId;
        grav.isSource = next.gravity->isSource != 0u;
        grav.owner = static_cast<ecs::OrbitOwner>(next.gravity->owner);

        if (next.gravity->hasRails)
        {
            ecs::OrbitState& orbit = registry.Get<ecs::OrbitState>(next.entity);
            orbit.elements.semiMajorAxis = next.gravity->semiMajorAxis;
            orbit.elements.eccentricity = next.gravity->eccentricity;
            orbit.elements.inclination = next.gravity->inclination;
            orbit.elements.longitudeAscNode = next.gravity->longitudeAscNode;
            orbit.elements.argPeriapsis = next.gravity->argPeriapsis;
            orbit.elements.trueAnomaly = next.gravity->trueAnomaly;
            orbit.primaryMu = next.gravity->primaryMu;
            orbit.primaryBodyId = next.gravity->primaryBodyId;
            orbit.epoch = next.gravity->railsEpoch;
        }
        if (next.clock)
        {
            ecs::RelativisticBody& relativistic =
                registry.Get<ecs::RelativisticBody>(next.entity);
            ecs::RelativisticClock& clock =
                registry.Get<ecs::RelativisticClock>(next.entity);
            relativistic.momentum = next.clock->momentum;
            relativistic.restMass = next.clock->restMass;
            clock.coordinateTime = next.clock->coordinateTime;
            clock.properTimeDeviation = next.clock->properTimeDeviation;
        }
    }
    frames.RebuildFromFrames(std::move(replacementFrames));

    SnapshotApplyResult result;
    result.accepted = true;
    return result;
}

} // namespace sim
