#pragma once

#include "on_foot_controller.h"
#include "../asset/cooked_assembly.h"
#include "../ecs/components.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace gameplay
{

enum class PlayerControlContext : uint8_t
{
    Uninitialized,
    Ship,
    OnFoot
};

enum class PilotPossessionStatus : uint8_t
{
    Success,
    InvalidArgument,
    InvalidTopology,
    NotInitialized,
    WrongContext,
    SeatUnavailable,
    SpawnBlocked,
    OutOfRange,
    NotFacing,
    TopologyMismatch,
    StaleCollisionSnapshot,
    CollisionFailure,
    InternalError
};

const char* PilotPossessionStatusName(PilotPossessionStatus status);

struct AssemblySocketFrame
{
    uint32_t stableIndex = asset::kAssemblyNoIndex;
    uint32_t moduleIndex = asset::kAssemblyNoIndex;
    core::Vec3d position;
    core::Vec3f forward = { 0.0f, 0.0f, 1.0f };
    core::Vec3f up = { 0.0f, 1.0f, 0.0f };
};

struct PilotSeatBinding
{
    asset::Sha256Digest topologySha256;
    uint32_t interactionIndex = asset::kAssemblyNoIndex;
    uint32_t availableStateIndex = asset::kAssemblyNoIndex;
    uint32_t occupiedStateIndex = asset::kAssemblyNoIndex;
    AssemblySocketFrame seat;
    AssemblySocketFrame spawn;
};

struct PilotSeatBindingResult
{
    PilotPossessionStatus status = PilotPossessionStatus::InvalidArgument;
    PilotSeatBinding binding;
    std::string error;

    bool Succeeded() const
    {
        return status == PilotPossessionStatus::Success;
    }
};

struct PilotPossessionConfig
{
    double capsuleRadiusMeters = 0.35;
    double capsuleHalfSegmentMeters = 0.45;
    double spawnFloorClearanceMeters = 0.02;
    double eyeOffsetFromCapsuleCenterMeters = 0.65;
    double maximumSeatUseDistanceMeters = 2.5;
    float minimumSeatFacingDot = 0.25f;
    float lookSensitivityDegreesPerCount = 0.15f;
    float maximumLookPitchDegrees = 89.0f;
    double uniformRootScaleTolerance = 1.0e-5;
};

struct PlayerPossessionState
{
    PlayerControlContext context = PlayerControlContext::Uninitialized;
    OnFootState onFoot;
    float localViewYawDegrees = 0.0f;
    float localViewPitchDegrees = 0.0f;
};

struct PilotPossessionResult
{
    PilotPossessionStatus status = PilotPossessionStatus::InvalidArgument;
    scene::InteriorCollisionStatus collisionStatus =
        scene::InteriorCollisionStatus::Success;
    PlayerPossessionState state;
    std::string error;

    bool Succeeded() const
    {
        return status == PilotPossessionStatus::Success;
    }
};

struct OnFootCameraPose
{
    core::Vec3d position;
    core::Vec3f forward = { 0.0f, 0.0f, 1.0f };
    core::Vec3f up = { 0.0f, 1.0f, 0.0f };
};

struct OnFootCameraResult
{
    PilotPossessionStatus status = PilotPossessionStatus::InvalidArgument;
    OnFootCameraPose pose;
    std::string error;

    bool Succeeded() const
    {
        return status == PilotPossessionStatus::Success;
    }
};

PilotSeatBindingResult ResolvePilotSeatBinding(
    const asset::CookedAssembly& assembly,
    std::string_view seatInteractionId = "pilot_seat",
    std::string_view spawnSocketId = "pilot_exit_spawn");

PilotPossessionResult InitializeShipPossession(
    const PilotSeatBinding& binding,
    uint32_t currentSeatStateIndex,
    const PilotPossessionConfig& config = {});

// Stages a complete transition but never mutates the authored seat runtime.
// The host commits the seat state first and assigns result.state only after that
// transaction succeeds, so a rejected host mutation cannot transfer control.
PilotPossessionResult StagePilotExit(
    const PilotSeatBinding& binding,
    const PlayerPossessionState& source,
    uint32_t currentSeatStateIndex,
    const scene::AssemblyInteriorCollisionSnapshot& collision,
    const PilotPossessionConfig& config = {});

PilotPossessionResult StagePilotEntry(
    const PilotSeatBinding& binding,
    const PlayerPossessionState& source,
    uint32_t currentSeatStateIndex,
    const scene::AssemblyInteriorCollisionSnapshot& collision,
    const PilotPossessionConfig& config = {});

PilotPossessionResult ApplyOnFootLook(
    const PlayerPossessionState& source,
    float pointerDeltaX,
    float pointerDeltaY,
    const PilotPossessionConfig& config = {});

core::Vec3d OnFootViewForward(const PlayerPossessionState& state);

OnFootCameraResult BuildOnFootCameraPose(
    const PlayerPossessionState& state,
    const ecs::Transform& assemblyRoot,
    const PilotPossessionConfig& config = {});

bool IsValidPossessionRoot(
    const ecs::Transform& root,
    double uniformScaleTolerance = 1.0e-5);

bool AssemblyLocalPointToWorld(
    const ecs::Transform& root,
    const core::Vec3d& local,
    core::Vec3d& world,
    double uniformScaleTolerance = 1.0e-5);

bool WorldPointToAssemblyLocal(
    const ecs::Transform& root,
    const core::Vec3d& world,
    core::Vec3d& local,
    double uniformScaleTolerance = 1.0e-5);

bool AssemblyLocalDirectionToWorld(
    const ecs::Transform& root,
    const core::Vec3d& local,
    core::Vec3f& world,
    double uniformScaleTolerance = 1.0e-5);

bool WorldDirectionToAssemblyLocal(
    const ecs::Transform& root,
    const core::Vec3f& world,
    core::Vec3d& local,
    double uniformScaleTolerance = 1.0e-5);

inline bool OwnsShipInput(const PlayerPossessionState& state)
{
    return state.context == PlayerControlContext::Ship;
}

inline bool OwnsOnFootInput(const PlayerPossessionState& state)
{
    return state.context == PlayerControlContext::OnFoot;
}

} // namespace gameplay
