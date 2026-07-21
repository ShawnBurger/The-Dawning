// =============================================================================
// sim/ftl_system.cpp - atomic ECS adapter for FTL frame transitions
// =============================================================================

#include "ftl_system.h"

#include <cmath>
#include <limits>

namespace sim
{

namespace
{

bool IsFinite(const Vec3d& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool IsSafeLocal(const Vec3d& value)
{
    return IsFinite(value) &&
           std::fabs(value.x) <= kSectorSize &&
           std::fabs(value.y) <= kSectorSize &&
           std::fabs(value.z) <= kSectorSize;
}

bool IsUsableFrame(const Frame& frame)
{
    return ValidSector(frame.origin) && IsCanonical(frame.origin) &&
           IsFinite(frame.velocity);
}

bool TryNarrow(const Vec3d& value, core::Vec3f& out)
{
    constexpr double kFloatMax =
        static_cast<double>((std::numeric_limits<float>::max)());
    if (!IsFinite(value) || std::fabs(value.x) > kFloatMax ||
        std::fabs(value.y) > kFloatMax || std::fabs(value.z) > kFloatMax)
        return false;

    const core::Vec3f candidate{
        static_cast<float>(value.x),
        static_cast<float>(value.y),
        static_cast<float>(value.z),
    };
    if (!std::isfinite(candidate.x) || !std::isfinite(candidate.y) ||
        !std::isfinite(candidate.z))
        return false;

    out = candidate;
    return true;
}

} // namespace

FtlTransitionResult TryTeleportEntity(ecs::Registry& registry,
                                      ecs::Entity entity,
                                      const FrameGraph& frames,
                                      FrameId destinationFrame,
                                      const MouthTransform& mouth)
{
    FtlTransitionResult result;

    ecs::Transform* transform = registry.TryGet<ecs::Transform>(entity);
    ecs::SpatialFrame* spatialFrame = registry.TryGet<ecs::SpatialFrame>(entity);
    ecs::RigidBody* rigidBody = registry.TryGet<ecs::RigidBody>(entity);
    if (!transform || !spatialFrame || !rigidBody)
        return result;

    const FrameId sourceFrame = static_cast<FrameId>(spatialFrame->frameId);
    if (sourceFrame >= frames.FrameCount() ||
        destinationFrame >= frames.FrameCount() ||
        !IsSafeLocal(transform->position) ||
        !IsSafeLocal(rigidBody->prevPosition) ||
        !IsUsableFrame(frames.GetFrame(sourceFrame)) ||
        !IsUsableFrame(frames.GetFrame(destinationFrame)))
        return result;

    const Body sourceBody{ sourceFrame, transform->position,
                           rigidBody->linearVelocity };
    const Body sourceHistory{ sourceFrame, rigidBody->prevPosition, Vec3d{} };

    TeleportState input;
    input.position = frames.ResolveWorldPos(sourceBody);
    input.orientation = transform->rotation;
    input.linearVelocity = frames.ResolveWorldVel(sourceBody);
    input.angularVelocity = Vec3d::FromFloat(rigidBody->angularVelocity);
    input.forceAccum = rigidBody->forceAccum;
    input.torqueAccum = Vec3d::FromFloat(rigidBody->torqueAccum);
    input.prevPosition = frames.ResolveWorldPos(sourceHistory);
    input.prevRotation = rigidBody->prevRotation;

    ecs::RelativisticBody* relativistic =
        registry.TryGet<ecs::RelativisticBody>(entity);
    if (relativistic)
        input.momentum = relativistic->momentum;

    TeleportState output;
    if (!TryApplyTeleport(input, mouth, output))
        return result;

    const Frame& destination = frames.GetFrame(destinationFrame);
    const Vec3d localPosition = Separation(destination.origin, output.position);
    const Vec3d localPrevious = Separation(destination.origin, output.prevPosition);
    const Vec3d localVelocity = output.linearVelocity - destination.velocity;
    if (!IsSafeLocal(localPosition) || !IsSafeLocal(localPrevious) ||
        !IsFinite(localVelocity))
        return result;

    core::Vec3f angularVelocity;
    core::Vec3f torqueAccum;
    if (!TryNarrow(output.angularVelocity, angularVelocity) ||
        !TryNarrow(output.torqueAccum, torqueAccum))
        return result;

    // Stage every changed component before publishing any write. This makes all
    // validation failures above bit-preserving for the registry.
    ecs::Transform nextTransform = *transform;
    nextTransform.position = localPosition;
    nextTransform.rotation = output.orientation;

    ecs::RigidBody nextRigidBody = *rigidBody;
    nextRigidBody.linearVelocity = localVelocity;
    nextRigidBody.angularVelocity = angularVelocity;
    nextRigidBody.forceAccum = output.forceAccum;
    nextRigidBody.torqueAccum = torqueAccum;
    nextRigidBody.prevPosition = localPrevious;
    nextRigidBody.prevRotation = output.prevRotation;

    ecs::SpatialFrame nextSpatialFrame = *spatialFrame;
    nextSpatialFrame.frameId = destinationFrame;

    ecs::RelativisticBody nextRelativistic;
    if (relativistic)
    {
        nextRelativistic = *relativistic;
        nextRelativistic.momentum = output.momentum;
    }

    ecs::GravitationalBody* gravitational =
        registry.TryGet<ecs::GravitationalBody>(entity);
    ecs::GravitationalBody nextGravitational;
    if (gravitational)
    {
        nextGravitational = *gravitational;
        nextGravitational.owner = ecs::OrbitOwner::NBodyActive;
    }

    *transform = nextTransform;
    *rigidBody = nextRigidBody;
    *spatialFrame = nextSpatialFrame;
    if (relativistic)
        *relativistic = nextRelativistic;
    if (gravitational)
        *gravitational = nextGravitational;

    result.accepted = true;
    result.resetRenderHistory = true;
    result.drainFixedAccumulator = true;
    return result;
}

} // namespace sim
