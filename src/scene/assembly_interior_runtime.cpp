#include "assembly_interior_runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace scene
{

namespace
{

constexpr double kProgressEpsilon = 1.0e-12;

AssemblyInteriorResult Failure(
    AssemblyInteriorStatus status,
    std::string error,
    uint32_t stableIndex = asset::kAssemblyNoIndex)
{
    AssemblyInteriorResult result;
    result.status = status;
    result.stableIndex = stableIndex;
    result.error = std::move(error);
    return result;
}

bool IsFinite(const core::Vec3d& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool IsFinite(const core::Vec3f& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool IsFinite(const core::Quatf& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

bool IsFinite(const ecs::Transform& value)
{
    return IsFinite(value.position) && IsFinite(value.rotation) &&
           IsFinite(value.scale) && value.scale.x > 0.0f &&
           value.scale.y > 0.0f && value.scale.z > 0.0f;
}

bool IsNonZero(const asset::Sha256Digest& digest)
{
    for (uint8_t value : digest.bytes)
        if (value != 0)
            return true;
    return false;
}

bool IsMovingInteraction(asset::AssemblyInteractionType type)
{
    return type == asset::AssemblyInteractionType::Airlock ||
           type == asset::AssemblyInteractionType::Door ||
           type == asset::AssemblyInteractionType::Elevator ||
           type == asset::AssemblyInteractionType::Hatch;
}

uint32_t FindState(
    const asset::AssemblyInteraction& interaction,
    std::string_view name)
{
    const auto it = std::find(
        interaction.states.begin(), interaction.states.end(), name);
    if (it == interaction.states.end())
        return asset::kAssemblyNoIndex;
    return static_cast<uint32_t>(it - interaction.states.begin());
}

core::Vec3f ToFloat3(const std::array<double, 3>& value)
{
    return {
        static_cast<float>(value[0]),
        static_cast<float>(value[1]),
        static_cast<float>(value[2])
    };
}

core::Vec3f ScaleLocal(
    const core::Vec3f& value,
    const core::Vec3f& scale)
{
    return { value.x * scale.x, value.y * scale.y, value.z * scale.z };
}

core::Vec3d LocalPointToWorld(
    const ecs::Transform& module,
    const std::array<double, 3>& local)
{
    const core::Vec3f scaled = ScaleLocal(ToFloat3(local), module.scale);
    return module.position + core::Vec3d::FromFloat(
        module.rotation.Rotate(scaled));
}

bool ValidConfig(const AssemblyInteriorConfig& config)
{
    return std::isfinite(config.linearSpeedMetersPerSecond) &&
           std::isfinite(config.angularSpeedDegreesPerSecond) &&
           config.linearSpeedMetersPerSecond > 0.0 &&
           config.angularSpeedDegreesPerSecond > 0.0;
}

} // namespace

const char* AssemblyInteriorStatusName(AssemblyInteriorStatus status)
{
    switch (status)
    {
    case AssemblyInteriorStatus::Success: return "success";
    case AssemblyInteriorStatus::InvalidArgument: return "invalid argument";
    case AssemblyInteriorStatus::InvalidTopology: return "invalid topology";
    case AssemblyInteriorStatus::NotInitialized: return "not initialized";
    case AssemblyInteriorStatus::NotFound: return "not found";
    case AssemblyInteriorStatus::Locked: return "locked";
    case AssemblyInteriorStatus::InvalidSnapshot: return "invalid snapshot";
    case AssemblyInteriorStatus::TopologyMismatch: return "topology mismatch";
    case AssemblyInteriorStatus::AllocationFailure: return "allocation failure";
    case AssemblyInteriorStatus::InternalError: return "internal error";
    default: return "unknown";
    }
}

AssemblyInteriorResult AssemblyInteriorRuntime::Initialize(
    std::shared_ptr<const asset::CookedAssembly> assembly,
    std::span<const PreparedAssemblyModule> modules,
    std::span<const PreparedAssemblyMovingPart> movingParts)
{
    if (IsInitialized())
    {
        return Failure(
            AssemblyInteriorStatus::InvalidArgument,
            "interior runtime is already initialized");
    }
    if (!assembly || !IsNonZero(assembly->sourceManifestSha256) ||
        modules.size() != assembly->modules.size() ||
        movingParts.size() != assembly->movingParts.size())
    {
        return Failure(
            AssemblyInteriorStatus::InvalidArgument,
            "assembly or prepared topology is incomplete");
    }

    try
    {
        std::vector<PreparedAssemblyModule> stagedModules(
            modules.begin(), modules.end());
        std::vector<PreparedAssemblyMovingPart> stagedMovingParts(
            movingParts.begin(), movingParts.end());
        std::vector<InteractionRecord> stagedInteractions;
        std::vector<ecs::Transform> stagedTransforms;
        std::map<std::string, uint32_t, std::less<>> stagedIndices;
        stagedInteractions.reserve(assembly->interactions.size());
        stagedTransforms.reserve(assembly->movingParts.size());

        for (size_t i = 0; i < stagedModules.size(); ++i)
        {
            if (stagedModules[i].stableIndex != i ||
                !IsFinite(stagedModules[i].worldTransform))
            {
                return Failure(
                    AssemblyInteriorStatus::InvalidTopology,
                    "prepared module transform or stable index is invalid",
                    static_cast<uint32_t>(i));
            }
        }
        for (size_t i = 0; i < stagedMovingParts.size(); ++i)
        {
            const asset::AssemblyMovingPart& source = assembly->movingParts[i];
            const PreparedAssemblyMovingPart& prepared = stagedMovingParts[i];
            const core::Vec3f axis = ToFloat3(source.axis);
            if (prepared.stableIndex != i || !IsFinite(prepared.worldTransform) ||
                source.moduleIndex >= assembly->modules.size() ||
                source.interactionIndex >= assembly->interactions.size() ||
                prepared.moduleIndex != source.moduleIndex ||
                prepared.interactionIndex != source.interactionIndex ||
                assembly->interactions[source.interactionIndex].movingPartIndex != i ||
                (source.motionType != asset::AssemblyMotionType::Linear &&
                 source.motionType != asset::AssemblyMotionType::Rotational) ||
                !IsFinite(ToFloat3(source.pivotMeters)) || !IsFinite(axis) ||
                axis.LengthSq() < 1.0e-8f || !std::isfinite(source.travel) ||
                std::abs(source.travel) <= kProgressEpsilon)
            {
                return Failure(
                    AssemblyInteriorStatus::InvalidTopology,
                    "prepared moving-part topology is invalid",
                    static_cast<uint32_t>(i));
            }
            stagedTransforms.push_back(prepared.worldTransform);
        }

        for (size_t i = 0; i < assembly->interactions.size(); ++i)
        {
            const asset::AssemblyInteraction& interaction =
                assembly->interactions[i];
            if (interaction.id.empty() ||
                !stagedIndices.emplace(
                    interaction.id, static_cast<uint32_t>(i)).second ||
                interaction.moduleIndex >= assembly->modules.size() ||
                interaction.socketIndex >= assembly->sockets.size() ||
                assembly->sockets[interaction.socketIndex].moduleIndex !=
                    interaction.moduleIndex ||
                interaction.states.empty() ||
                interaction.initialStateIndex >= interaction.states.size())
            {
                return Failure(
                    AssemblyInteriorStatus::InvalidTopology,
                    "interaction identity, socket, or initial state is invalid",
                    static_cast<uint32_t>(i));
            }

            InteractionRecord record;
            record.stateIndex = interaction.initialStateIndex;
            if (IsMovingInteraction(interaction.type))
            {
                if (interaction.movingPartIndex >= assembly->movingParts.size() ||
                    interaction.portalIndex >= assembly->portals.size() ||
                    assembly->movingParts[interaction.movingPartIndex].interactionIndex != i ||
                    assembly->portals[interaction.portalIndex].closureInteraction != i)
                {
                    return Failure(
                        AssemblyInteriorStatus::InvalidTopology,
                        "moving interaction ownership is not reciprocal",
                        static_cast<uint32_t>(i));
                }
                record.closedState = FindState(interaction, "closed");
                record.openingState = FindState(interaction, "opening");
                record.openState = FindState(interaction, "open");
                record.closingState = FindState(interaction, "closing");
                record.lockedState = FindState(interaction, "locked");
                if (record.closedState == asset::kAssemblyNoIndex ||
                    record.openingState == asset::kAssemblyNoIndex ||
                    record.openState == asset::kAssemblyNoIndex ||
                    record.closingState == asset::kAssemblyNoIndex ||
                    record.lockedState == asset::kAssemblyNoIndex)
                {
                    return Failure(
                        AssemblyInteriorStatus::InvalidTopology,
                        "moving interaction is missing required state vocabulary",
                        static_cast<uint32_t>(i));
                }
                if (record.stateIndex == record.openState ||
                    record.stateIndex == record.closingState)
                {
                    record.motionProgress = 1.0;
                }
                else if (record.stateIndex != record.closedState &&
                         record.stateIndex != record.openingState &&
                         record.stateIndex != record.lockedState)
                {
                    return Failure(
                        AssemblyInteriorStatus::InvalidTopology,
                        "moving interaction initial state is not executable",
                        static_cast<uint32_t>(i));
                }
            }
            else if (interaction.movingPartIndex != asset::kAssemblyNoIndex ||
                     interaction.portalIndex != asset::kAssemblyNoIndex)
            {
                return Failure(
                    AssemblyInteriorStatus::InvalidTopology,
                    "nonmoving interaction owns a moving part or portal",
                    static_cast<uint32_t>(i));
            }
            stagedInteractions.push_back(record);
        }

        for (size_t i = 0; i < assembly->portals.size(); ++i)
        {
            const asset::AssemblyPortal& portal = assembly->portals[i];
            if (portal.closureInteraction >= assembly->interactions.size() ||
                assembly->interactions[portal.closureInteraction].portalIndex != i ||
                !portal.sealable || !portal.navLink)
            {
                return Failure(
                    AssemblyInteriorStatus::InvalidTopology,
                    "portal closure topology is invalid",
                    static_cast<uint32_t>(i));
            }
        }

        m_assembly = std::move(assembly);
        m_modules = std::move(stagedModules);
        m_movingParts = std::move(stagedMovingParts);
        m_interactions = std::move(stagedInteractions);
        m_movingTransforms = std::move(stagedTransforms);
        m_interactionIndices = std::move(stagedIndices);
        RebuildMovingTransforms();
        return { AssemblyInteriorStatus::Success };
    }
    catch (const std::bad_alloc&)
    {
        Shutdown();
        return Failure(
            AssemblyInteriorStatus::AllocationFailure,
            "allocation failure while initializing interior runtime");
    }
    catch (...)
    {
        Shutdown();
        return Failure(
            AssemblyInteriorStatus::InternalError,
            "unexpected interior runtime initialization failure");
    }
}

void AssemblyInteriorRuntime::Shutdown() noexcept
{
    m_interactionIndices.clear();
    m_movingTransforms.clear();
    m_interactions.clear();
    m_movingParts.clear();
    m_modules.clear();
    m_assembly.reset();
}

size_t AssemblyInteriorRuntime::PortalCount() const
{
    return m_assembly ? m_assembly->portals.size() : 0u;
}

AssemblyInteriorResult AssemblyInteriorRuntime::ActivateInteraction(
    uint32_t stableIndex)
{
    if (!m_assembly)
    {
        return Failure(
            AssemblyInteriorStatus::NotInitialized,
            "interior runtime is not initialized");
    }
    if (stableIndex >= m_interactions.size())
    {
        return Failure(
            AssemblyInteriorStatus::NotFound,
            "interaction stable index was not found",
            stableIndex);
    }

    InteractionRecord& record = m_interactions[stableIndex];
    const asset::AssemblyInteraction& interaction =
        m_assembly->interactions[stableIndex];
    if (IsMovingInteraction(interaction.type))
    {
        if (record.stateIndex == record.lockedState)
        {
            return Failure(
                AssemblyInteriorStatus::Locked,
                "interaction is locked",
                stableIndex);
        }
        if (record.stateIndex == record.closedState ||
            record.stateIndex == record.closingState)
        {
            record.stateIndex = record.openingState;
        }
        else if (record.stateIndex == record.openState ||
                 record.stateIndex == record.openingState)
        {
            record.stateIndex = record.closingState;
        }
        else
        {
            return Failure(
                AssemblyInteriorStatus::InvalidTopology,
                "moving interaction entered an unknown state",
                stableIndex);
        }
    }
    else
    {
        record.stateIndex = static_cast<uint32_t>(
            (static_cast<size_t>(record.stateIndex) + 1u) %
            interaction.states.size());
    }

    AssemblyInteriorResult result;
    result.status = AssemblyInteriorStatus::Success;
    result.stableIndex = stableIndex;
    result.changed = true;
    return result;
}

AssemblyInteriorResult AssemblyInteriorRuntime::ActivateInteraction(
    std::string_view id)
{
    if (!m_assembly)
    {
        return Failure(
            AssemblyInteriorStatus::NotInitialized,
            "interior runtime is not initialized");
    }
    const auto it = m_interactionIndices.find(id);
    if (it == m_interactionIndices.end())
    {
        return Failure(
            AssemblyInteriorStatus::NotFound,
            "interaction id was not found");
    }
    return ActivateInteraction(it->second);
}

AssemblyInteriorResult AssemblyInteriorRuntime::ActivateNearest(
    const AssemblyInteractionQuery& query)
{
    if (!m_assembly)
    {
        return Failure(
            AssemblyInteriorStatus::NotInitialized,
            "interior runtime is not initialized");
    }
    if (!IsFinite(query.worldPosition) || !IsFinite(query.worldForward) ||
        !std::isfinite(query.maxDistanceMeters) ||
        !std::isfinite(query.minimumForwardDot) ||
        query.maxDistanceMeters <= 0.0 || query.minimumForwardDot < -1.0f ||
        query.minimumForwardDot > 1.0f ||
        query.worldForward.LengthSq() < 1.0e-8f)
    {
        return Failure(
            AssemblyInteriorStatus::InvalidArgument,
            "interaction query is invalid");
    }

    const core::Vec3f forward = query.worldForward.Normalized();
    const double maxDistanceSq =
        query.maxDistanceMeters * query.maxDistanceMeters;
    double bestDistanceSq = (std::numeric_limits<double>::max)();
    uint32_t bestIndex = asset::kAssemblyNoIndex;
    for (size_t i = 0; i < m_assembly->interactions.size(); ++i)
    {
        const asset::AssemblyInteraction& interaction =
            m_assembly->interactions[i];
        const asset::AssemblySocket& socket =
            m_assembly->sockets[interaction.socketIndex];
        const core::Vec3d socketPosition = LocalPointToWorld(
            m_modules[interaction.moduleIndex].worldTransform,
            socket.positionMeters);
        const core::Vec3d offset = socketPosition - query.worldPosition;
        const double distanceSq = offset.LengthSq();
        if (!std::isfinite(distanceSq) || distanceSq > maxDistanceSq)
            continue;

        float dot = 1.0f;
        if (distanceSq > 1.0e-12)
        {
            dot = forward.Dot(offset.ToFloat().Normalized());
            if (dot < query.minimumForwardDot)
                continue;
        }
        if (distanceSq + 1.0e-12 < bestDistanceSq)
        {
            bestDistanceSq = distanceSq;
            bestIndex = static_cast<uint32_t>(i);
        }
    }
    if (bestIndex == asset::kAssemblyNoIndex)
    {
        return Failure(
            AssemblyInteriorStatus::NotFound,
            "no interaction is within the query volume");
    }
    return ActivateInteraction(bestIndex);
}

AssemblyInteriorResult AssemblyInteriorRuntime::Advance(
    double dt,
    const AssemblyInteriorConfig& config)
{
    if (!m_assembly)
    {
        return Failure(
            AssemblyInteriorStatus::NotInitialized,
            "interior runtime is not initialized");
    }
    if (!std::isfinite(dt) || dt < 0.0 || !ValidConfig(config))
    {
        return Failure(
            AssemblyInteriorStatus::InvalidArgument,
            "interior time step or motion rates are invalid");
    }

    // Validate the complete active batch before changing any interaction. A
    // caller-supplied extreme rate must not advance an earlier closure and then
    // reject a later one.
    for (size_t i = 0; i < m_interactions.size(); ++i)
    {
        const InteractionRecord& record = m_interactions[i];
        if (record.stateIndex != record.openingState &&
            record.stateIndex != record.closingState)
        {
            continue;
        }
        const asset::AssemblyInteraction& interaction =
            m_assembly->interactions[i];
        const asset::AssemblyMovingPart& part =
            m_assembly->movingParts[interaction.movingPartIndex];
        const double speed = part.motionType == asset::AssemblyMotionType::Linear
            ? config.linearSpeedMetersPerSecond
            : config.angularSpeedDegreesPerSecond;
        const double duration = std::abs(part.travel) / speed;
        if (!std::isfinite(duration) || duration <= 0.0)
        {
            return Failure(
                AssemblyInteriorStatus::InvalidArgument,
                "interior motion rates cannot represent an active duration",
                static_cast<uint32_t>(i));
        }
    }

    bool changed = false;
    for (size_t i = 0; i < m_interactions.size(); ++i)
    {
        InteractionRecord& record = m_interactions[i];
        const asset::AssemblyInteraction& interaction =
            m_assembly->interactions[i];
        if (!IsMovingInteraction(interaction.type))
            continue;
        if (record.stateIndex != record.openingState &&
            record.stateIndex != record.closingState)
        {
            continue;
        }

        const asset::AssemblyMovingPart& part =
            m_assembly->movingParts[interaction.movingPartIndex];
        const double speed = part.motionType == asset::AssemblyMotionType::Linear
            ? config.linearSpeedMetersPerSecond
            : config.angularSpeedDegreesPerSecond;
        const double duration = std::abs(part.travel) / speed;
        const double previous = record.motionProgress;
        if (record.stateIndex == record.openingState)
        {
            record.motionProgress = (std::min)(1.0, previous + dt / duration);
            if (record.motionProgress >= 1.0 - kProgressEpsilon)
            {
                record.motionProgress = 1.0;
                record.stateIndex = record.openState;
            }
        }
        else
        {
            record.motionProgress = (std::max)(0.0, previous - dt / duration);
            if (record.motionProgress <= kProgressEpsilon)
            {
                record.motionProgress = 0.0;
                record.stateIndex = record.closedState;
            }
        }
        changed = changed || record.motionProgress != previous;
    }

    if (changed)
        RebuildMovingTransforms();
    AssemblyInteriorResult result;
    result.status = AssemblyInteriorStatus::Success;
    result.changed = changed;
    return result;
}

bool AssemblyInteriorRuntime::IsPortalTraversable(uint32_t stableIndex) const
{
    if (!m_assembly || stableIndex >= m_assembly->portals.size())
        return false;
    const asset::AssemblyPortal& portal = m_assembly->portals[stableIndex];
    if (!portal.navLink || portal.closureInteraction >= m_interactions.size())
        return false;
    const InteractionRecord& closure =
        m_interactions[portal.closureInteraction];
    return closure.stateIndex == closure.openState &&
           closure.motionProgress == 1.0;
}

uint32_t AssemblyInteriorRuntime::InteractionIndex(std::string_view id) const
{
    if (!m_assembly)
        return asset::kAssemblyNoIndex;
    const auto it = m_interactionIndices.find(id);
    return it == m_interactionIndices.end()
        ? asset::kAssemblyNoIndex
        : it->second;
}

uint32_t AssemblyInteriorRuntime::InteractionPortalIndex(
    uint32_t stableIndex) const
{
    return m_assembly && stableIndex < m_assembly->interactions.size()
        ? m_assembly->interactions[stableIndex].portalIndex
        : asset::kAssemblyNoIndex;
}

std::string_view AssemblyInteriorRuntime::InteractionStateName(
    uint32_t stableIndex) const
{
    if (!m_assembly || stableIndex >= m_interactions.size())
        return {};
    const asset::AssemblyInteraction& interaction =
        m_assembly->interactions[stableIndex];
    const uint32_t state = m_interactions[stableIndex].stateIndex;
    return state < interaction.states.size()
        ? std::string_view(interaction.states[state])
        : std::string_view{};
}

uint32_t AssemblyInteriorRuntime::InteractionStateIndex(
    uint32_t stableIndex) const
{
    return stableIndex < m_interactions.size()
        ? m_interactions[stableIndex].stateIndex
        : asset::kAssemblyNoIndex;
}

double AssemblyInteriorRuntime::InteractionMotionProgress(
    uint32_t stableIndex) const
{
    return stableIndex < m_interactions.size()
        ? m_interactions[stableIndex].motionProgress
        : 0.0;
}

const ecs::Transform* AssemblyInteriorRuntime::MovingPartTransform(
    uint32_t stableIndex) const
{
    return stableIndex < m_movingTransforms.size()
        ? &m_movingTransforms[stableIndex]
        : nullptr;
}

AssemblyInteriorSnapshot AssemblyInteriorRuntime::CaptureSnapshot() const
{
    AssemblyInteriorSnapshot snapshot;
    if (!m_assembly)
        return snapshot;
    snapshot.topologySha256 = m_assembly->sourceManifestSha256;
    snapshot.interactions.reserve(m_interactions.size());
    for (const InteractionRecord& record : m_interactions)
    {
        snapshot.interactions.push_back({
            record.stateIndex,
            record.motionProgress
        });
    }
    return snapshot;
}

bool AssemblyInteriorRuntime::ValidateSnapshotRecord(
    uint32_t interactionIndex,
    const AssemblyInteractionSnapshot& snapshot) const
{
    if (!m_assembly || interactionIndex >= m_interactions.size() ||
        snapshot.stateIndex >= m_assembly->interactions[interactionIndex].states.size() ||
        !std::isfinite(snapshot.motionProgress) ||
        snapshot.motionProgress < 0.0 || snapshot.motionProgress > 1.0)
    {
        return false;
    }

    const asset::AssemblyInteraction& interaction =
        m_assembly->interactions[interactionIndex];
    const InteractionRecord& vocabulary = m_interactions[interactionIndex];
    if (!IsMovingInteraction(interaction.type))
        return snapshot.motionProgress == 0.0;
    if (snapshot.stateIndex == vocabulary.closedState)
        return snapshot.motionProgress == 0.0;
    if (snapshot.stateIndex == vocabulary.openState)
        return snapshot.motionProgress == 1.0;
    if (snapshot.stateIndex == vocabulary.openingState)
        return snapshot.motionProgress >= 0.0 && snapshot.motionProgress < 1.0;
    if (snapshot.stateIndex == vocabulary.closingState)
        return snapshot.motionProgress > 0.0 && snapshot.motionProgress <= 1.0;
    if (snapshot.stateIndex == vocabulary.lockedState)
        return true;
    return false;
}

AssemblyInteriorResult AssemblyInteriorRuntime::ApplySnapshot(
    const AssemblyInteriorSnapshot& snapshot)
{
    if (!m_assembly)
    {
        return Failure(
            AssemblyInteriorStatus::NotInitialized,
            "interior runtime is not initialized");
    }
    if (snapshot.topologySha256 != m_assembly->sourceManifestSha256)
    {
        return Failure(
            AssemblyInteriorStatus::TopologyMismatch,
            "interior snapshot topology does not match the live assembly");
    }
    if (snapshot.interactions.size() != m_interactions.size())
    {
        return Failure(
            AssemblyInteriorStatus::InvalidSnapshot,
            "interior snapshot interaction count is invalid");
    }
    for (size_t i = 0; i < snapshot.interactions.size(); ++i)
    {
        if (!ValidateSnapshotRecord(
                static_cast<uint32_t>(i), snapshot.interactions[i]))
        {
            return Failure(
                AssemblyInteriorStatus::InvalidSnapshot,
                "interior snapshot contains an invalid interaction state",
                static_cast<uint32_t>(i));
        }
    }

    bool changed = false;
    for (size_t i = 0; i < snapshot.interactions.size(); ++i)
    {
        changed = changed ||
            m_interactions[i].stateIndex != snapshot.interactions[i].stateIndex ||
            m_interactions[i].motionProgress !=
                snapshot.interactions[i].motionProgress;
    }
    for (size_t i = 0; i < snapshot.interactions.size(); ++i)
    {
        m_interactions[i].stateIndex = snapshot.interactions[i].stateIndex;
        m_interactions[i].motionProgress =
            snapshot.interactions[i].motionProgress;
    }
    if (changed)
        RebuildMovingTransforms();

    AssemblyInteriorResult result;
    result.status = AssemblyInteriorStatus::Success;
    result.changed = changed;
    return result;
}

void AssemblyInteriorRuntime::RebuildMovingTransforms()
{
    if (!m_assembly)
        return;
    for (size_t i = 0; i < m_assembly->movingParts.size(); ++i)
    {
        const asset::AssemblyMovingPart& part = m_assembly->movingParts[i];
        const ecs::Transform& module =
            m_modules[part.moduleIndex].worldTransform;
        const ecs::Transform& closed =
            m_movingParts[i].worldTransform;
        ecs::Transform transformed = closed;
        const double progress =
            m_interactions[part.interactionIndex].motionProgress;
        const core::Vec3f axis = ToFloat3(part.axis).Normalized();

        if (part.motionType == asset::AssemblyMotionType::Linear)
        {
            const core::Vec3f localTravel = ScaleLocal(
                axis * static_cast<float>(part.travel * progress),
                module.scale);
            transformed.position = closed.position + core::Vec3d::FromFloat(
                module.rotation.Rotate(localTravel));
        }
        else
        {
            const float radians = static_cast<float>(
                part.travel * progress * (core::PI / 180.0));
            const core::Quatf localDelta =
                core::Quatf::FromAxisAngle(axis, radians).Normalized();
            const core::Quatf worldDelta = (
                module.rotation * localDelta *
                module.rotation.Conjugate()).Normalized();
            const core::Vec3d pivot =
                LocalPointToWorld(module, part.pivotMeters);
            const core::Vec3f offset = (closed.position - pivot).ToFloat();
            transformed.position = pivot + core::Vec3d::FromFloat(
                worldDelta.Rotate(offset));
            transformed.rotation =
                (worldDelta * closed.rotation).Normalized();
        }
        m_movingTransforms[i] = transformed;
    }
}

} // namespace scene
