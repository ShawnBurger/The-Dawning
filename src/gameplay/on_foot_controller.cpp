#include "on_foot_controller.h"

#include <algorithm>
#include <cmath>

namespace gameplay
{
namespace
{

constexpr double kEpsilon = 1.0e-10;
constexpr double kMaximumMagnitude = 1.0e12;
constexpr double kMaximumControllerRate = 1.0e6;

bool Finite(double value)
{
    return std::isfinite(value) && std::abs(value) <= kMaximumMagnitude;
}

bool Finite(const core::Vec3d& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool NonZero(const asset::Sha256Digest& digest)
{
    for (uint8_t value : digest.bytes)
        if (value != 0)
            return true;
    return false;
}

bool ValidState(const OnFootState& state)
{
    const bool topologyConsistent = state.collisionRevision == 0
        ? !NonZero(state.collisionTopologySha256)
        : NonZero(state.collisionTopologySha256);
    return Finite(state.capsule.center) && Finite(state.capsule.radius) &&
           Finite(state.capsule.halfSegment) && state.capsule.radius > 0.0 &&
           state.capsule.halfSegment >= 0.0 && Finite(state.velocity) &&
           topologyConsistent;
}

bool ValidCommand(const OnFootCommand& command)
{
    return Finite(command.moveRight) && Finite(command.moveForward) &&
           command.moveRight >= -1.0 && command.moveRight <= 1.0 &&
           command.moveForward >= -1.0 && command.moveForward <= 1.0 &&
           Finite(command.viewForward);
}

bool ValidConfig(const OnFootControllerConfig& config)
{
    const auto positiveRate = [](double value) {
        return std::isfinite(value) && value > 0.0 &&
               value <= kMaximumControllerRate;
    };
    return positiveRate(config.maximumCapsuleRadiusMeters) &&
           positiveRate(config.maximumCapsuleHalfSegmentMeters) &&
           positiveRate(config.walkSpeedMetersPerSecond) &&
           positiveRate(config.sprintSpeedMetersPerSecond) &&
           config.sprintSpeedMetersPerSecond >=
               config.walkSpeedMetersPerSecond &&
           positiveRate(config.groundAccelerationMetersPerSecondSq) &&
           positiveRate(config.groundBrakingMetersPerSecondSq) &&
           positiveRate(config.airAccelerationMetersPerSecondSq) &&
           positiveRate(config.gravityMetersPerSecondSq) &&
           positiveRate(config.jumpSpeedMetersPerSecond) &&
           positiveRate(config.maximumRiseSpeedMetersPerSecond) &&
           config.maximumRiseSpeedMetersPerSecond >=
               config.jumpSpeedMetersPerSecond &&
           positiveRate(config.terminalFallSpeedMetersPerSecond) &&
           std::isfinite(config.minimumTimeStepSeconds) &&
           std::isfinite(config.maximumTimeStepSeconds) &&
           config.minimumTimeStepSeconds > 0.0 &&
           config.maximumTimeStepSeconds >= config.minimumTimeStepSeconds &&
           config.maximumTimeStepSeconds <= 1.0 &&
           scene::IsValidInteriorLocomotionConfig(config.locomotion);
}

struct VectorIntegration
{
    core::Vec3d velocity;
    core::Vec3d displacement;
};

VectorIntegration IntegrateToward(
    const core::Vec3d& start,
    const core::Vec3d& target,
    double rate,
    double dt)
{
    VectorIntegration result;
    const core::Vec3d delta = target - start;
    const double distance = delta.Length();
    if (distance <= kEpsilon)
    {
        result.velocity = target;
        result.displacement = target * dt;
        return result;
    }
    const core::Vec3d direction = delta / distance;
    const double timeToTarget = distance / rate;
    if (timeToTarget >= dt)
    {
        result.velocity = start + direction * (rate * dt);
        result.displacement = (start + result.velocity) * (0.5 * dt);
        return result;
    }
    result.velocity = target;
    result.displacement =
        (start + target) * (0.5 * timeToTarget) +
        target * (dt - timeToTarget);
    return result;
}

struct ScalarIntegration
{
    double velocity = 0.0;
    double displacement = 0.0;
};

ScalarIntegration IntegrateToward(
    double start,
    double target,
    double rate,
    double dt)
{
    const VectorIntegration vector = IntegrateToward(
        { start, 0.0, 0.0 }, { target, 0.0, 0.0 }, rate, dt);
    return { vector.velocity.x, vector.displacement.x };
}

core::Vec3d ClampPlanarSpeed(core::Vec3d velocity, double maximum)
{
    velocity.y = 0.0;
    const double speed = velocity.Length();
    if (speed > maximum && speed > kEpsilon)
        velocity *= maximum / speed;
    return velocity;
}

OnFootStepResult Failure(
    OnFootControllerStatus status,
    const OnFootState& source,
    scene::InteriorCollisionStatus collisionStatus =
        scene::InteriorCollisionStatus::Success)
{
    OnFootStepResult result;
    result.status = status;
    result.collisionStatus = collisionStatus;
    result.state = source;
    return result;
}

} // namespace

const char* OnFootControllerStatusName(OnFootControllerStatus status)
{
    switch (status)
    {
    case OnFootControllerStatus::Success: return "success";
    case OnFootControllerStatus::InvalidArgument: return "invalid_argument";
    case OnFootControllerStatus::TopologyMismatch: return "topology_mismatch";
    case OnFootControllerStatus::StaleCollisionSnapshot: return "stale_collision_snapshot";
    case OnFootControllerStatus::CollisionFailure: return "collision_failure";
    case OnFootControllerStatus::InternalError: return "internal_error";
    }
    return "unknown";
}

OnFootStepResult StepOnFootController(
    const scene::AssemblyInteriorCollisionSnapshot& collision,
    const OnFootState& source,
    const OnFootCommand& command,
    double dt,
    const OnFootControllerConfig& config)
{
    if (!collision.collisionWorld || collision.revision == 0 ||
        !NonZero(collision.topologySha256) || !ValidState(source) ||
        !ValidCommand(command) || !ValidConfig(config) || !std::isfinite(dt) ||
        dt < config.minimumTimeStepSeconds ||
        dt > config.maximumTimeStepSeconds)
    {
        return Failure(OnFootControllerStatus::InvalidArgument, source);
    }
    if (source.capsule.radius > config.maximumCapsuleRadiusMeters ||
        source.capsule.halfSegment >
            config.maximumCapsuleHalfSegmentMeters ||
        source.capsule.radius >
            kMaximumMagnitude - config.locomotion.skinWidth)
    {
        return Failure(OnFootControllerStatus::InvalidArgument, source);
    }
    if (source.collisionRevision != 0 &&
        source.collisionTopologySha256 != collision.topologySha256)
    {
        return Failure(OnFootControllerStatus::TopologyMismatch, source);
    }
    if (source.collisionRevision > collision.revision)
    {
        return Failure(
            OnFootControllerStatus::StaleCollisionSnapshot, source);
    }

    const double rawDemandLength = std::hypot(
        command.moveRight, command.moveForward);
    core::Vec3d planarDemand{
        command.moveRight, 0.0, command.moveForward
    };
    if (rawDemandLength > 1.0)
        planarDemand *= 1.0 / rawDemandLength;

    core::Vec3d forward{
        command.viewForward.x, 0.0, command.viewForward.z
    };
    if (planarDemand.LengthSq() > kEpsilon)
    {
        if (forward.LengthSq() <= kEpsilon)
            return Failure(OnFootControllerStatus::InvalidArgument, source);
        forward = forward.Normalized();
    }
    else
    {
        forward = { 0.0, 0.0, 1.0 };
    }
    const core::Vec3d right{ forward.z, 0.0, -forward.x };
    const core::Vec3d movementDirection =
        right * planarDemand.x + forward * planarDemand.z;

    scene::InteriorLocomotionConfig probeConfig = config.locomotion;
    if (source.velocity.y > 0.0)
        probeConfig.groundProbeDistance = 0.0;
    const scene::InteriorCapsuleMotion probe =
        collision.collisionWorld->MoveCapsule(
            source.capsule, {}, probeConfig);
    if (!probe.Succeeded())
    {
        return Failure(
            OnFootControllerStatus::CollisionFailure,
            source,
            probe.status);
    }

    scene::InteriorCapsule capsule = source.capsule;
    capsule.center = probe.center;
    bool grounded = probe.grounded;
    const bool jumpEdge = command.jumpDown && !source.jumpHeld;
    const bool jumped = grounded && jumpEdge;

    core::Vec3d horizontalStart = ClampPlanarSpeed(
        { source.velocity.x, 0.0, source.velocity.z },
        config.sprintSpeedMetersPerSecond);
    const double targetSpeed = command.sprint
        ? config.sprintSpeedMetersPerSecond
        : config.walkSpeedMetersPerSecond;
    core::Vec3d horizontalTarget = movementDirection * targetSpeed;
    double horizontalRate = config.airAccelerationMetersPerSecondSq;
    if (grounded)
    {
        horizontalRate = movementDirection.LengthSq() > kEpsilon
            ? config.groundAccelerationMetersPerSecondSq
            : config.groundBrakingMetersPerSecondSq;
    }
    else if (movementDirection.LengthSq() <= kEpsilon)
    {
        horizontalTarget = horizontalStart;
    }
    const VectorIntegration horizontal = IntegrateToward(
        horizontalStart, horizontalTarget, horizontalRate, dt);

    ScalarIntegration vertical;
    if (jumped)
    {
        grounded = false;
        vertical = IntegrateToward(
            config.jumpSpeedMetersPerSecond,
            -config.terminalFallSpeedMetersPerSecond,
            config.gravityMetersPerSecondSq,
            dt);
    }
    else if (!grounded)
    {
        const double start = (std::clamp)(
            source.velocity.y,
            -config.terminalFallSpeedMetersPerSecond,
            config.maximumRiseSpeedMetersPerSecond);
        vertical = IntegrateToward(
            start,
            -config.terminalFallSpeedMetersPerSecond,
            config.gravityMetersPerSecondSq,
            dt);
    }

    const core::Vec3d desiredDisplacement{
        horizontal.displacement.x,
        vertical.displacement,
        horizontal.displacement.z
    };
    if (!Finite(capsule.center + desiredDisplacement))
        return Failure(OnFootControllerStatus::InvalidArgument, source);

    const scene::InteriorCapsuleMotion motion =
        collision.collisionWorld->MoveCapsule(
            capsule, desiredDisplacement, config.locomotion);
    if (!motion.Succeeded())
    {
        return Failure(
            OnFootControllerStatus::CollisionFailure,
            source,
            motion.status);
    }

    core::Vec3d velocity{
        horizontal.velocity.x,
        vertical.velocity,
        horizontal.velocity.z
    };
    if (motion.blocked)
    {
        velocity.x = motion.achievedDisplacement.x / dt;
        velocity.z = motion.achievedDisplacement.z / dt;
    }
    core::Vec3d clampedHorizontal = ClampPlanarSpeed(
        { velocity.x, 0.0, velocity.z },
        config.sprintSpeedMetersPerSecond);
    velocity.x = clampedHorizontal.x;
    velocity.z = clampedHorizontal.z;
    if (motion.grounded && velocity.y <= 0.0)
        velocity.y = 0.0;
    else if (desiredDisplacement.y > 0.0 &&
             motion.achievedDisplacement.y + config.locomotion.minimumMoveDistance <
                 desiredDisplacement.y)
        velocity.y = 0.0;

    if (!Finite(motion.center) || !Finite(velocity))
        return Failure(OnFootControllerStatus::InternalError, source);

    OnFootStepResult result;
    result.status = OnFootControllerStatus::Success;
    result.state = source;
    result.state.capsule.center = motion.center;
    result.state.velocity = velocity;
    result.state.collisionTopologySha256 = collision.topologySha256;
    result.state.collisionRevision = collision.revision;
    result.state.groundStableId = motion.grounded
        ? motion.groundStableId
        : 0;
    result.state.grounded = motion.grounded;
    result.state.jumpHeld = command.jumpDown;
    result.achievedDisplacement = motion.center - source.capsule.center;
    result.jumped = jumped;
    result.blocked = probe.depenetrated || probe.blocked || motion.blocked;
    result.stepped = motion.stepped;
    return result;
}

} // namespace gameplay
