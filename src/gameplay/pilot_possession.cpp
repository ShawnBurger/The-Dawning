#include "pilot_possession.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace gameplay
{

namespace
{

constexpr double kVectorEpsilon = 1.0e-12;
constexpr float kBasisEpsilon = 1.0e-6f;
constexpr float kSocketOrthogonalityTolerance = 0.01f;
constexpr float kUprightSocketDot = 0.999f;
constexpr float kQuaternionUnitTolerance = 1.0e-3f;
constexpr double kMinimumRootScale = 1.0e-6;
constexpr double kMaximumRootScale = 1.0e6;
constexpr double kMaximumLocalMagnitude = 1.0e7;

bool Finite(double value)
{
    return std::isfinite(value);
}

bool Finite(float value)
{
    return std::isfinite(value);
}

bool Finite(const core::Vec3d& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Finite(const core::Vec3f& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Finite(const core::Quatf& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z) &&
           Finite(value.w);
}

bool BoundedLocal(const core::Vec3d& value)
{
    return Finite(value) &&
           std::abs(value.x) <= kMaximumLocalMagnitude &&
           std::abs(value.y) <= kMaximumLocalMagnitude &&
           std::abs(value.z) <= kMaximumLocalMagnitude;
}

bool NonZero(const asset::Sha256Digest& digest)
{
    for (const uint8_t value : digest.bytes)
        if (value != 0)
            return true;
    return false;
}

bool ValidConfig(const PilotPossessionConfig& config)
{
    return Finite(config.capsuleRadiusMeters) &&
           Finite(config.capsuleHalfSegmentMeters) &&
           Finite(config.spawnFloorClearanceMeters) &&
           Finite(config.eyeOffsetFromCapsuleCenterMeters) &&
           Finite(config.maximumSeatUseDistanceMeters) &&
           Finite(config.minimumSeatFacingDot) &&
           Finite(config.lookSensitivityDegreesPerCount) &&
           Finite(config.maximumLookPitchDegrees) &&
           Finite(config.uniformRootScaleTolerance) &&
           config.capsuleRadiusMeters > 0.0 &&
           config.capsuleRadiusMeters <= 1.0 &&
           config.capsuleHalfSegmentMeters >= 0.0 &&
           config.capsuleHalfSegmentMeters <= 1.5 &&
           config.spawnFloorClearanceMeters >= 0.0 &&
           config.spawnFloorClearanceMeters <= 0.25 &&
           config.eyeOffsetFromCapsuleCenterMeters >= 0.0 &&
           config.eyeOffsetFromCapsuleCenterMeters <= 2.0 &&
           config.maximumSeatUseDistanceMeters > 0.0 &&
           config.maximumSeatUseDistanceMeters <= 10.0 &&
           config.minimumSeatFacingDot >= -1.0f &&
           config.minimumSeatFacingDot <= 1.0f &&
           config.lookSensitivityDegreesPerCount > 0.0f &&
           config.lookSensitivityDegreesPerCount <= 10.0f &&
           config.maximumLookPitchDegrees > 0.0f &&
           config.maximumLookPitchDegrees < 90.0f &&
           config.uniformRootScaleTolerance > 0.0 &&
           config.uniformRootScaleTolerance <= 0.01;
}

bool ValidSocketFrame(const AssemblySocketFrame& frame)
{
    if (frame.stableIndex == asset::kAssemblyNoIndex ||
        frame.moduleIndex == asset::kAssemblyNoIndex ||
        !BoundedLocal(frame.position) || !Finite(frame.forward) ||
        !Finite(frame.up))
    {
        return false;
    }
    const float forwardLengthSq = frame.forward.LengthSq();
    const float upLengthSq = frame.up.LengthSq();
    return std::abs(forwardLengthSq - 1.0f) <= 1.0e-3f &&
           std::abs(upLengthSq - 1.0f) <= 1.0e-3f &&
           std::abs(frame.forward.Dot(frame.up)) <=
               kSocketOrthogonalityTolerance;
}

bool ValidBinding(const PilotSeatBinding& binding)
{
    return NonZero(binding.topologySha256) &&
           binding.interactionIndex != asset::kAssemblyNoIndex &&
           binding.availableStateIndex != asset::kAssemblyNoIndex &&
           binding.occupiedStateIndex != asset::kAssemblyNoIndex &&
           binding.availableStateIndex != binding.occupiedStateIndex &&
           ValidSocketFrame(binding.seat) && ValidSocketFrame(binding.spawn) &&
           binding.seat.stableIndex != binding.spawn.stableIndex &&
           binding.seat.moduleIndex == binding.spawn.moduleIndex &&
           binding.seat.up.Dot({ 0.0f, 1.0f, 0.0f }) >=
               kUprightSocketDot &&
           binding.spawn.up.Dot({ 0.0f, 1.0f, 0.0f }) >=
               kUprightSocketDot &&
           std::abs(binding.spawn.forward.y) <= 1.0e-3f;
}

bool ValidOnFootState(const PlayerPossessionState& state)
{
    return state.context == PlayerControlContext::OnFoot &&
           Finite(state.localViewYawDegrees) &&
           Finite(state.localViewPitchDegrees) &&
           std::abs(state.localViewYawDegrees) <= 180.0f &&
           std::abs(state.localViewPitchDegrees) < 90.0f &&
           BoundedLocal(state.onFoot.capsule.center) &&
           Finite(state.onFoot.capsule.radius) &&
           Finite(state.onFoot.capsule.halfSegment) &&
           state.onFoot.capsule.radius > 0.0 &&
           state.onFoot.capsule.halfSegment >= 0.0 &&
           BoundedLocal(state.onFoot.velocity) &&
           NonZero(state.onFoot.collisionTopologySha256) &&
           state.onFoot.collisionRevision != 0;
}

bool ValidSourceState(const PlayerPossessionState& state)
{
    if (!Finite(state.localViewYawDegrees) ||
        !Finite(state.localViewPitchDegrees))
    {
        return false;
    }
    if (state.context == PlayerControlContext::Ship)
        return true;
    return ValidOnFootState(state);
}

PilotSeatBindingResult BindingFailure(
    PilotPossessionStatus status,
    std::string error)
{
    PilotSeatBindingResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

PilotPossessionResult TransitionFailure(
    PilotPossessionStatus status,
    const PlayerPossessionState& source,
    std::string error,
    scene::InteriorCollisionStatus collisionStatus =
        scene::InteriorCollisionStatus::Success)
{
    PilotPossessionResult result;
    result.status = status;
    result.collisionStatus = collisionStatus;
    result.state = source;
    result.error = std::move(error);
    return result;
}

bool ToFloat3(
    const std::array<double, 3>& values,
    core::Vec3f& converted)
{
    constexpr double maximum = (std::numeric_limits<float>::max)();
    for (const double value : values)
        if (!Finite(value) || std::abs(value) > maximum)
            return false;
    converted = {
        static_cast<float>(values[0]),
        static_cast<float>(values[1]),
        static_cast<float>(values[2])
    };
    return true;
}

bool BuildModuleTransform(
    const asset::AssemblyTransform& source,
    ecs::Transform& transform)
{
    core::Vec3f scale;
    const core::Vec3d position{
        source.positionMeters[0],
        source.positionMeters[1],
        source.positionMeters[2]
    };
    if (!BoundedLocal(position) ||
        !Finite(source.rotationEulerDegrees[0]) ||
        !Finite(source.rotationEulerDegrees[1]) ||
        !Finite(source.rotationEulerDegrees[2]) ||
        !ToFloat3(source.scale, scale) || scale.x <= 0.0f ||
        scale.y <= 0.0f || scale.z <= 0.0f)
    {
        return false;
    }
    transform.position = position;
    const float pitch = static_cast<float>(
        std::remainder(source.rotationEulerDegrees[0], 360.0) *
        core::DEG_TO_RAD);
    const float yaw = static_cast<float>(
        std::remainder(source.rotationEulerDegrees[1], 360.0) *
        core::DEG_TO_RAD);
    const float roll = static_cast<float>(
        std::remainder(source.rotationEulerDegrees[2], 360.0) *
        core::DEG_TO_RAD);
    transform.rotation = core::Quatf::FromEuler(pitch, yaw, roll).Normalized();
    transform.scale = scale;
    return Finite(transform.position) && Finite(transform.rotation);
}

bool BuildSocketFrame(
    const asset::CookedAssembly& assembly,
    uint32_t socketIndex,
    AssemblySocketFrame& frame)
{
    if (socketIndex >= assembly.sockets.size())
        return false;
    const asset::AssemblySocket& socket = assembly.sockets[socketIndex];
    if (socket.moduleIndex >= assembly.modules.size())
        return false;

    ecs::Transform module;
    core::Vec3f socketPosition;
    core::Vec3f socketForward;
    core::Vec3f socketUp;
    if (!BuildModuleTransform(
            assembly.modules[socket.moduleIndex].transform, module) ||
        !ToFloat3(socket.positionMeters, socketPosition) ||
        !ToFloat3(socket.forward, socketForward) ||
        !ToFloat3(socket.up, socketUp) ||
        socketForward.LengthSq() <= kBasisEpsilon ||
        socketUp.LengthSq() <= kBasisEpsilon)
    {
        return false;
    }

    socketForward = socketForward.Normalized();
    socketUp = socketUp.Normalized();
    if (std::abs(socketForward.Dot(socketUp)) >
        kSocketOrthogonalityTolerance)
    {
        return false;
    }
    const core::Vec3f scaledPosition{
        socketPosition.x * module.scale.x,
        socketPosition.y * module.scale.y,
        socketPosition.z * module.scale.z
    };
    if (!Finite(scaledPosition) ||
        std::abs(static_cast<double>(scaledPosition.x)) > kMaximumLocalMagnitude ||
        std::abs(static_cast<double>(scaledPosition.y)) > kMaximumLocalMagnitude ||
        std::abs(static_cast<double>(scaledPosition.z)) > kMaximumLocalMagnitude)
    {
        return false;
    }

    frame.stableIndex = socketIndex;
    frame.moduleIndex = socket.moduleIndex;
    frame.position = module.position + core::Vec3d::FromFloat(
        module.rotation.Rotate(scaledPosition));
    frame.forward = module.rotation.Rotate(socketForward).Normalized();
    const core::Vec3f upCandidate =
        module.rotation.Rotate(socketUp).Normalized();
    const core::Vec3f right = upCandidate.Cross(frame.forward).Normalized();
    if (right.LengthSq() <= kBasisEpsilon)
        return false;
    frame.up = frame.forward.Cross(right).Normalized();
    return BoundedLocal(frame.position) && ValidSocketFrame(frame);
}

uint32_t FindUniqueSocket(
    const asset::CookedAssembly& assembly,
    std::string_view id)
{
    uint32_t found = asset::kAssemblyNoIndex;
    for (size_t i = 0; i < assembly.sockets.size(); ++i)
    {
        if (assembly.sockets[i].id != id)
            continue;
        if (found != asset::kAssemblyNoIndex)
            return asset::kAssemblyNoIndex;
        found = static_cast<uint32_t>(i);
    }
    return found;
}

uint32_t FindUniqueInteraction(
    const asset::CookedAssembly& assembly,
    std::string_view id)
{
    uint32_t found = asset::kAssemblyNoIndex;
    for (size_t i = 0; i < assembly.interactions.size(); ++i)
    {
        if (assembly.interactions[i].id != id)
            continue;
        if (found != asset::kAssemblyNoIndex)
            return asset::kAssemblyNoIndex;
        found = static_cast<uint32_t>(i);
    }
    return found;
}

uint32_t FindUniqueState(
    const asset::AssemblyInteraction& interaction,
    std::string_view name)
{
    uint32_t found = asset::kAssemblyNoIndex;
    for (size_t i = 0; i < interaction.states.size(); ++i)
    {
        if (interaction.states[i] != name)
            continue;
        if (found != asset::kAssemblyNoIndex)
            return asset::kAssemblyNoIndex;
        found = static_cast<uint32_t>(i);
    }
    return found;
}

float WrapDegrees(float angle)
{
    angle = std::remainder(angle, 360.0f);
    if (angle == -180.0f)
        angle = 180.0f;
    return angle;
}

core::Vec3d ForwardFromAngles(float yawDegrees, float pitchDegrees)
{
    const double yaw = static_cast<double>(yawDegrees) * core::DEG_TO_RAD;
    const double pitch = static_cast<double>(pitchDegrees) * core::DEG_TO_RAD;
    const double cosPitch = std::cos(pitch);
    return {
        std::sin(yaw) * cosPitch,
        std::sin(pitch),
        std::cos(yaw) * cosPitch
    };
}

void AnglesFromForward(
    const core::Vec3f& source,
    float& yawDegrees,
    float& pitchDegrees)
{
    const core::Vec3f forward = source.Normalized();
    yawDegrees = std::atan2(forward.x, forward.z) * core::RAD_TO_DEG;
    pitchDegrees = std::asin((std::max)(-1.0f, (std::min)(1.0f, forward.y))) *
        core::RAD_TO_DEG;
}

PilotPossessionStatus ValidateCollisionSnapshot(
    const PilotSeatBinding& binding,
    const scene::AssemblyInteriorCollisionSnapshot& collision)
{
    if (!collision.collisionWorld || collision.revision == 0 ||
        !NonZero(collision.topologySha256))
    {
        return PilotPossessionStatus::InvalidArgument;
    }
    return collision.topologySha256 == binding.topologySha256
        ? PilotPossessionStatus::Success
        : PilotPossessionStatus::TopologyMismatch;
}

} // namespace

const char* PilotPossessionStatusName(PilotPossessionStatus status)
{
    switch (status)
    {
    case PilotPossessionStatus::Success: return "success";
    case PilotPossessionStatus::InvalidArgument: return "invalid_argument";
    case PilotPossessionStatus::InvalidTopology: return "invalid_topology";
    case PilotPossessionStatus::NotInitialized: return "not_initialized";
    case PilotPossessionStatus::WrongContext: return "wrong_context";
    case PilotPossessionStatus::SeatUnavailable: return "seat_unavailable";
    case PilotPossessionStatus::SpawnBlocked: return "spawn_blocked";
    case PilotPossessionStatus::OutOfRange: return "out_of_range";
    case PilotPossessionStatus::NotFacing: return "not_facing";
    case PilotPossessionStatus::TopologyMismatch: return "topology_mismatch";
    case PilotPossessionStatus::StaleCollisionSnapshot:
        return "stale_collision_snapshot";
    case PilotPossessionStatus::CollisionFailure: return "collision_failure";
    case PilotPossessionStatus::InternalError: return "internal_error";
    }
    return "unknown";
}

PilotSeatBindingResult ResolvePilotSeatBinding(
    const asset::CookedAssembly& assembly,
    std::string_view seatInteractionId,
    std::string_view spawnSocketId)
{
    if (seatInteractionId.empty() || spawnSocketId.empty())
    {
        return BindingFailure(
            PilotPossessionStatus::InvalidArgument,
            "pilot seat and spawn IDs must be nonempty");
    }
    if (!NonZero(assembly.sourceManifestSha256))
    {
        return BindingFailure(
            PilotPossessionStatus::InvalidTopology,
            "assembly topology digest is unavailable");
    }

    const uint32_t interactionIndex =
        FindUniqueInteraction(assembly, seatInteractionId);
    const uint32_t spawnIndex = FindUniqueSocket(assembly, spawnSocketId);
    if (interactionIndex == asset::kAssemblyNoIndex ||
        spawnIndex == asset::kAssemblyNoIndex)
    {
        return BindingFailure(
            PilotPossessionStatus::InvalidTopology,
            "pilot seat or spawn socket identity was not resolved exactly once");
    }

    const asset::AssemblyInteraction& interaction =
        assembly.interactions[interactionIndex];
    if (interaction.type != asset::AssemblyInteractionType::Seat ||
        interaction.moduleIndex >= assembly.modules.size() ||
        interaction.socketIndex >= assembly.sockets.size() ||
        interaction.movingPartIndex != asset::kAssemblyNoIndex ||
        interaction.portalIndex != asset::kAssemblyNoIndex ||
        interaction.states.size() != 2)
    {
        return BindingFailure(
            PilotPossessionStatus::InvalidTopology,
            "pilot interaction is not a two-state nonmoving seat");
    }

    const asset::AssemblySocket& seatSocket =
        assembly.sockets[interaction.socketIndex];
    const asset::AssemblySocket& spawnSocket = assembly.sockets[spawnIndex];
    if (seatSocket.type != asset::AssemblySocketType::Interaction ||
        spawnSocket.type != asset::AssemblySocketType::Spawn ||
        seatSocket.moduleIndex != interaction.moduleIndex ||
        spawnSocket.moduleIndex != interaction.moduleIndex ||
        assembly.modules[interaction.moduleIndex].role !=
            asset::AssemblyModuleRole::Interior)
    {
        return BindingFailure(
            PilotPossessionStatus::InvalidTopology,
            "pilot seat and spawn socket ownership is invalid");
    }

    const uint32_t available = FindUniqueState(interaction, "available");
    const uint32_t occupied = FindUniqueState(interaction, "occupied");
    if (available == asset::kAssemblyNoIndex ||
        occupied == asset::kAssemblyNoIndex || available == occupied)
    {
        return BindingFailure(
            PilotPossessionStatus::InvalidTopology,
            "pilot seat requires exact available and occupied states");
    }

    PilotSeatBindingResult result;
    result.binding.topologySha256 = assembly.sourceManifestSha256;
    result.binding.interactionIndex = interactionIndex;
    result.binding.availableStateIndex = available;
    result.binding.occupiedStateIndex = occupied;
    if (!BuildSocketFrame(
            assembly, interaction.socketIndex, result.binding.seat) ||
        !BuildSocketFrame(assembly, spawnIndex, result.binding.spawn) ||
        result.binding.seat.up.Dot({ 0.0f, 1.0f, 0.0f }) <
            kUprightSocketDot ||
        result.binding.spawn.up.Dot({ 0.0f, 1.0f, 0.0f }) <
            kUprightSocketDot ||
        std::abs(result.binding.spawn.forward.y) > 1.0e-3f)
    {
        return BindingFailure(
            PilotPossessionStatus::InvalidTopology,
            "pilot seat or spawn socket frame is invalid for upright locomotion");
    }
    result.status = PilotPossessionStatus::Success;
    return result;
}

PilotPossessionResult InitializeShipPossession(
    const PilotSeatBinding& binding,
    uint32_t currentSeatStateIndex,
    const PilotPossessionConfig& config)
{
    const PlayerPossessionState source;
    if (!ValidBinding(binding) || !ValidConfig(config))
    {
        return TransitionFailure(
            PilotPossessionStatus::InvalidArgument,
            source,
            "pilot binding or possession configuration is invalid");
    }
    if (currentSeatStateIndex != binding.occupiedStateIndex)
    {
        return TransitionFailure(
            PilotPossessionStatus::SeatUnavailable,
            source,
            "pilot seat is not occupied during ship possession initialization");
    }
    PilotPossessionResult result;
    result.status = PilotPossessionStatus::Success;
    result.state.context = PlayerControlContext::Ship;
    return result;
}

PilotPossessionResult StagePilotExit(
    const PilotSeatBinding& binding,
    const PlayerPossessionState& source,
    uint32_t currentSeatStateIndex,
    const scene::AssemblyInteriorCollisionSnapshot& collision,
    const PilotPossessionConfig& config)
{
    if (!ValidBinding(binding) || !ValidConfig(config) ||
        !ValidSourceState(source))
    {
        return TransitionFailure(
            PilotPossessionStatus::InvalidArgument,
            source,
            "pilot exit input is invalid");
    }
    if (source.context != PlayerControlContext::Ship)
    {
        return TransitionFailure(
            PilotPossessionStatus::WrongContext,
            source,
            "pilot exit requires ship possession");
    }
    if (currentSeatStateIndex != binding.occupiedStateIndex)
    {
        return TransitionFailure(
            PilotPossessionStatus::SeatUnavailable,
            source,
            "pilot seat is not occupied");
    }
    const PilotPossessionStatus collisionValidation =
        ValidateCollisionSnapshot(binding, collision);
    if (collisionValidation != PilotPossessionStatus::Success)
    {
        return TransitionFailure(
            collisionValidation,
            source,
            "pilot exit collision snapshot is invalid");
    }

    PlayerPossessionState staged = source;
    staged.context = PlayerControlContext::OnFoot;
    staged.onFoot = {};
    staged.onFoot.capsule.radius = config.capsuleRadiusMeters;
    staged.onFoot.capsule.halfSegment = config.capsuleHalfSegmentMeters;
    staged.onFoot.capsule.center = binding.spawn.position + core::Vec3d{
        0.0,
        config.capsuleRadiusMeters + config.capsuleHalfSegmentMeters +
            config.spawnFloorClearanceMeters,
        0.0
    };
    staged.onFoot.collisionTopologySha256 = collision.topologySha256;
    staged.onFoot.collisionRevision = collision.revision;
    AnglesFromForward(
        binding.spawn.forward,
        staged.localViewYawDegrees,
        staged.localViewPitchDegrees);
    if (!ValidOnFootState(staged))
    {
        return TransitionFailure(
            PilotPossessionStatus::InvalidArgument,
            source,
            "pilot spawn produced an invalid on-foot state");
    }

    try
    {
        std::vector<scene::InteriorCapsuleOverlap> overlaps;
        const scene::InteriorCollisionStatus overlapStatus =
            collision.collisionWorld->OverlapCapsule(
                staged.onFoot.capsule, 0.0, overlaps);
        if (overlapStatus != scene::InteriorCollisionStatus::Success)
        {
            return TransitionFailure(
                PilotPossessionStatus::CollisionFailure,
                source,
                "pilot spawn overlap query failed",
                overlapStatus);
        }
        if (!overlaps.empty())
        {
            return TransitionFailure(
                PilotPossessionStatus::SpawnBlocked,
                source,
                "pilot spawn capsule is obstructed");
        }
    }
    catch (const std::bad_alloc&)
    {
        return TransitionFailure(
            PilotPossessionStatus::InternalError,
            source,
            "allocation failure while validating pilot spawn");
    }
    catch (...)
    {
        return TransitionFailure(
            PilotPossessionStatus::InternalError,
            source,
            "unexpected failure while validating pilot spawn");
    }

    PilotPossessionResult result;
    result.status = PilotPossessionStatus::Success;
    result.state = staged;
    return result;
}

PilotPossessionResult StagePilotEntry(
    const PilotSeatBinding& binding,
    const PlayerPossessionState& source,
    uint32_t currentSeatStateIndex,
    const scene::AssemblyInteriorCollisionSnapshot& collision,
    const PilotPossessionConfig& config)
{
    if (!ValidBinding(binding) || !ValidConfig(config) ||
        !ValidSourceState(source))
    {
        return TransitionFailure(
            PilotPossessionStatus::InvalidArgument,
            source,
            "pilot entry input is invalid");
    }
    if (source.context != PlayerControlContext::OnFoot)
    {
        return TransitionFailure(
            PilotPossessionStatus::WrongContext,
            source,
            "pilot entry requires on-foot possession");
    }
    if (currentSeatStateIndex != binding.availableStateIndex)
    {
        return TransitionFailure(
            PilotPossessionStatus::SeatUnavailable,
            source,
            "pilot seat is not available");
    }
    const PilotPossessionStatus collisionValidation =
        ValidateCollisionSnapshot(binding, collision);
    if (collisionValidation != PilotPossessionStatus::Success)
    {
        return TransitionFailure(
            collisionValidation,
            source,
            "pilot entry collision snapshot is invalid");
    }
    if (source.onFoot.collisionTopologySha256 != binding.topologySha256)
    {
        return TransitionFailure(
            PilotPossessionStatus::TopologyMismatch,
            source,
            "pilot entry collision topology does not match the player");
    }
    if (source.onFoot.collisionRevision > collision.revision)
    {
        return TransitionFailure(
            PilotPossessionStatus::StaleCollisionSnapshot,
            source,
            "pilot entry collision snapshot is older than the player state");
    }

    const core::Vec3d eye = source.onFoot.capsule.center + core::Vec3d{
        0.0, config.eyeOffsetFromCapsuleCenterMeters, 0.0
    };
    const core::Vec3d offset = binding.seat.position - eye;
    const double distanceSq = offset.LengthSq();
    const double maximumDistanceSq =
        config.maximumSeatUseDistanceMeters *
        config.maximumSeatUseDistanceMeters;
    if (!Finite(distanceSq) || distanceSq > maximumDistanceSq)
    {
        return TransitionFailure(
            PilotPossessionStatus::OutOfRange,
            source,
            "pilot seat is outside interaction range");
    }
    if (distanceSq > kVectorEpsilon)
    {
        const core::Vec3d forward = OnFootViewForward(source);
        const double facing = forward.Dot(offset.Normalized());
        if (!Finite(facing) || facing < config.minimumSeatFacingDot)
        {
            return TransitionFailure(
                PilotPossessionStatus::NotFacing,
                source,
                "player is not facing the pilot seat");
        }
    }

    PilotPossessionResult result;
    result.status = PilotPossessionStatus::Success;
    result.state = source;
    result.state.context = PlayerControlContext::Ship;
    return result;
}

PilotPossessionResult ApplyOnFootLook(
    const PlayerPossessionState& source,
    float pointerDeltaX,
    float pointerDeltaY,
    const PilotPossessionConfig& config)
{
    if (!ValidConfig(config) || !ValidSourceState(source) ||
        !Finite(pointerDeltaX) || !Finite(pointerDeltaY) ||
        std::abs(pointerDeltaX) > 1.0e6f ||
        std::abs(pointerDeltaY) > 1.0e6f)
    {
        return TransitionFailure(
            PilotPossessionStatus::InvalidArgument,
            source,
            "on-foot look input is invalid");
    }
    if (source.context != PlayerControlContext::OnFoot)
    {
        return TransitionFailure(
            PilotPossessionStatus::WrongContext,
            source,
            "on-foot look requires on-foot possession");
    }

    PilotPossessionResult result;
    result.status = PilotPossessionStatus::Success;
    result.state = source;
    result.state.localViewYawDegrees = WrapDegrees(
        source.localViewYawDegrees +
        pointerDeltaX * config.lookSensitivityDegreesPerCount);
    result.state.localViewPitchDegrees = (std::max)(
        -config.maximumLookPitchDegrees,
        (std::min)(
            config.maximumLookPitchDegrees,
            source.localViewPitchDegrees -
                pointerDeltaY * config.lookSensitivityDegreesPerCount));
    return result;
}

core::Vec3d OnFootViewForward(const PlayerPossessionState& state)
{
    if (!Finite(state.localViewYawDegrees) ||
        !Finite(state.localViewPitchDegrees))
    {
        return {};
    }
    return ForwardFromAngles(
        state.localViewYawDegrees,
        state.localViewPitchDegrees).Normalized();
}

bool IsValidPossessionRoot(
    const ecs::Transform& root,
    double uniformScaleTolerance)
{
    if (!Finite(root.position) || !Finite(root.rotation) ||
        !Finite(root.scale) || !Finite(uniformScaleTolerance) ||
        uniformScaleTolerance <= 0.0 || uniformScaleTolerance > 0.01)
    {
        return false;
    }
    const float rotationLengthSq = root.rotation.LengthSq();
    if (!Finite(rotationLengthSq) ||
        std::abs(rotationLengthSq - 1.0f) > kQuaternionUnitTolerance)
    {
        return false;
    }
    const double x = root.scale.x;
    const double y = root.scale.y;
    const double z = root.scale.z;
    if (x < kMinimumRootScale || y < kMinimumRootScale ||
        z < kMinimumRootScale ||
        x > kMaximumRootScale || y > kMaximumRootScale ||
        z > kMaximumRootScale)
    {
        return false;
    }
    const double scale = (std::max)({ x, y, z });
    return std::abs(x - y) <= uniformScaleTolerance * scale &&
           std::abs(x - z) <= uniformScaleTolerance * scale;
}

bool AssemblyLocalPointToWorld(
    const ecs::Transform& root,
    const core::Vec3d& local,
    core::Vec3d& world,
    double uniformScaleTolerance)
{
    if (!IsValidPossessionRoot(root, uniformScaleTolerance) ||
        !BoundedLocal(local))
        return false;
    const double scale = root.scale.x;
    const core::Vec3d scaled = local * scale;
    constexpr double maximum = (std::numeric_limits<float>::max)();
    if (!Finite(scaled) || std::abs(scaled.x) > maximum ||
        std::abs(scaled.y) > maximum || std::abs(scaled.z) > maximum)
    {
        return false;
    }
    const core::Vec3f rotated = root.rotation.Normalized().Rotate(
        scaled.ToFloat());
    const core::Vec3d candidate =
        root.position + core::Vec3d::FromFloat(rotated);
    if (!Finite(candidate))
        return false;
    world = candidate;
    return true;
}

bool WorldPointToAssemblyLocal(
    const ecs::Transform& root,
    const core::Vec3d& world,
    core::Vec3d& local,
    double uniformScaleTolerance)
{
    if (!IsValidPossessionRoot(root, uniformScaleTolerance) || !Finite(world))
        return false;
    const core::Vec3d offset = world - root.position;
    constexpr double maximum = (std::numeric_limits<float>::max)();
    if (!Finite(offset) || std::abs(offset.x) > maximum ||
        std::abs(offset.y) > maximum || std::abs(offset.z) > maximum)
    {
        return false;
    }
    const core::Vec3f unrotated = root.rotation.Normalized().Conjugate().Rotate(
        offset.ToFloat());
    const core::Vec3d candidate =
        core::Vec3d::FromFloat(unrotated) / root.scale.x;
    if (!BoundedLocal(candidate))
        return false;
    local = candidate;
    return true;
}

bool AssemblyLocalDirectionToWorld(
    const ecs::Transform& root,
    const core::Vec3d& local,
    core::Vec3f& world,
    double uniformScaleTolerance)
{
    if (!IsValidPossessionRoot(root, uniformScaleTolerance) ||
        !BoundedLocal(local) || local.LengthSq() <= kVectorEpsilon)
    {
        return false;
    }
    constexpr double maximum = (std::numeric_limits<float>::max)();
    if (std::abs(local.x) > maximum || std::abs(local.y) > maximum ||
        std::abs(local.z) > maximum)
    {
        return false;
    }
    const core::Vec3f candidate = root.rotation.Normalized().Rotate(
        local.Normalized().ToFloat()).Normalized();
    if (!Finite(candidate) || candidate.LengthSq() <= kBasisEpsilon)
        return false;
    world = candidate;
    return true;
}

bool WorldDirectionToAssemblyLocal(
    const ecs::Transform& root,
    const core::Vec3f& world,
    core::Vec3d& local,
    double uniformScaleTolerance)
{
    if (!IsValidPossessionRoot(root, uniformScaleTolerance) || !Finite(world) ||
        world.LengthSq() <= kBasisEpsilon)
    {
        return false;
    }
    const core::Vec3f candidate = root.rotation.Normalized().Conjugate().Rotate(
        world.Normalized()).Normalized();
    if (!Finite(candidate) || candidate.LengthSq() <= kBasisEpsilon)
        return false;
    local = core::Vec3d::FromFloat(candidate);
    return true;
}

OnFootCameraResult BuildOnFootCameraPose(
    const PlayerPossessionState& state,
    const ecs::Transform& assemblyRoot,
    const PilotPossessionConfig& config)
{
    OnFootCameraResult result;
    if (!ValidConfig(config) || !ValidOnFootState(state) ||
        !IsValidPossessionRoot(
            assemblyRoot, config.uniformRootScaleTolerance))
    {
        result.status = PilotPossessionStatus::InvalidArgument;
        result.error = "on-foot camera state, root, or configuration is invalid";
        return result;
    }

    const core::Vec3d localForward = OnFootViewForward(state);
    core::Vec3d localRight =
        core::Vec3d{ 0.0, 1.0, 0.0 }.Cross(localForward);
    if (localRight.LengthSq() <= kVectorEpsilon)
    {
        result.status = PilotPossessionStatus::InvalidArgument;
        result.error = "on-foot view basis is degenerate";
        return result;
    }
    localRight = localRight.Normalized();
    const core::Vec3d localUp = localForward.Cross(localRight).Normalized();
    const core::Vec3d localEye = state.onFoot.capsule.center + core::Vec3d{
        0.0, config.eyeOffsetFromCapsuleCenterMeters, 0.0
    };
    if (!AssemblyLocalPointToWorld(
            assemblyRoot,
            localEye,
            result.pose.position,
            config.uniformRootScaleTolerance) ||
        !AssemblyLocalDirectionToWorld(
            assemblyRoot,
            localForward,
            result.pose.forward,
            config.uniformRootScaleTolerance) ||
        !AssemblyLocalDirectionToWorld(
            assemblyRoot,
            localUp,
            result.pose.up,
            config.uniformRootScaleTolerance))
    {
        result.status = PilotPossessionStatus::InvalidArgument;
        result.error = "on-foot camera pose could not cross the ship-root boundary";
        return result;
    }
    result.status = PilotPossessionStatus::Success;
    return result;
}

} // namespace gameplay
