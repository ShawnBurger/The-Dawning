#pragma once

#include "../scene/assembly_dynamic_collision_runtime.h"

#include <cstdint>

namespace gameplay
{

enum class OnFootControllerStatus : uint8_t
{
    Success,
    InvalidArgument,
    TopologyMismatch,
    StaleCollisionSnapshot,
    CollisionFailure,
    InternalError
};

const char* OnFootControllerStatusName(OnFootControllerStatus status);

struct OnFootCommand
{
    double moveRight = 0.0;
    double moveForward = 0.0;
    core::Vec3d viewForward = { 0.0, 0.0, 1.0 };
    bool sprint = false;
    bool jumpDown = false;
};

struct OnFootControllerConfig
{
    double walkSpeedMetersPerSecond = 4.5;
    double sprintSpeedMetersPerSecond = 6.5;
    double groundAccelerationMetersPerSecondSq = 24.0;
    double groundBrakingMetersPerSecondSq = 32.0;
    double airAccelerationMetersPerSecondSq = 6.0;
    double gravityMetersPerSecondSq = 18.0;
    double jumpSpeedMetersPerSecond = 6.0;
    double terminalFallSpeedMetersPerSecond = 55.0;
    double minimumTimeStepSeconds = 1.0e-6;
    double maximumTimeStepSeconds = 0.05;
    scene::InteriorLocomotionConfig locomotion;
};

struct OnFootState
{
    scene::InteriorCapsule capsule;
    core::Vec3d velocity;
    asset::Sha256Digest collisionTopologySha256;
    uint64_t collisionRevision = 0;
    uint64_t groundStableId = 0;
    bool grounded = false;
    bool jumpHeld = false;
};

struct OnFootStepResult
{
    OnFootControllerStatus status = OnFootControllerStatus::InvalidArgument;
    scene::InteriorCollisionStatus collisionStatus =
        scene::InteriorCollisionStatus::Success;
    OnFootState state;
    core::Vec3d achievedDisplacement;
    bool jumped = false;
    bool blocked = false;
    bool stepped = false;

    bool Succeeded() const
    {
        return status == OnFootControllerStatus::Success;
    }
};

// Pure fixed-step controller. The source state is never mutated; every failure
// returns it unchanged so the caller can commit only a complete accepted step.
OnFootStepResult StepOnFootController(
    const scene::AssemblyInteriorCollisionSnapshot& collision,
    const OnFootState& source,
    const OnFootCommand& command,
    double dt,
    const OnFootControllerConfig& config = {});

} // namespace gameplay
