#pragma once

#include "assembly_instantiator.h"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scene
{

enum class AssemblyInteriorStatus : uint8_t
{
    Success,
    InvalidArgument,
    InvalidTopology,
    NotInitialized,
    NotFound,
    Locked,
    InvalidSnapshot,
    TopologyMismatch,
    AllocationFailure,
    InternalError
};

const char* AssemblyInteriorStatusName(AssemblyInteriorStatus status);

struct AssemblyInteriorResult
{
    AssemblyInteriorStatus status = AssemblyInteriorStatus::InvalidArgument;
    uint32_t stableIndex = asset::kAssemblyNoIndex;
    bool changed = false;
    std::string error;

    bool Succeeded() const
    {
        return status == AssemblyInteriorStatus::Success;
    }
};

struct AssemblyInteriorConfig
{
    double linearSpeedMetersPerSecond = 1.0;
    double angularSpeedDegreesPerSecond = 90.0;
};

struct AssemblyInteractionSnapshot
{
    uint32_t stateIndex = asset::kAssemblyNoIndex;
    double motionProgress = 0.0;
};

struct AssemblyInteriorSnapshot
{
    asset::Sha256Digest topologySha256;
    std::vector<AssemblyInteractionSnapshot> interactions;
};

struct AssemblyInteractionQuery
{
    core::Vec3d assemblyPosition;
    core::Vec3f assemblyForward = { 0.0f, 0.0f, 1.0f };
    double maxDistanceMeters = 2.5;
    float minimumForwardDot = 0.25f;
};

// Reconstructs a moving part from immutable closed/module poses expressed in
// the same coordinate frame. Both presentation and collision use this motion
// law; callers choose world or assembly-local base transforms explicitly.
bool BuildAssemblyMovingPartTransform(
    const asset::AssemblyMovingPart& part,
    const ecs::Transform& module,
    const ecs::Transform& closed,
    double progress,
    ecs::Transform& transformed);

// Executes authored assembly interactions without owning ECS entities or GPU
// resources. Queries, modules, and moving transforms are assembly-local. All
// moving transforms are reconstructed from immutable closed poses, so stepping,
// reversal, root motion, and snapshot restoration never accumulate drift.
class AssemblyInteriorRuntime final
{
public:
    AssemblyInteriorRuntime() = default;
    AssemblyInteriorRuntime(const AssemblyInteriorRuntime&) = delete;
    AssemblyInteriorRuntime& operator=(const AssemblyInteriorRuntime&) = delete;

    AssemblyInteriorResult Initialize(
        std::shared_ptr<const asset::CookedAssembly> assembly,
        std::span<const PreparedAssemblyModule> modules,
        std::span<const PreparedAssemblyMovingPart> movingParts);

    void Shutdown() noexcept;

    bool IsInitialized() const { return m_assembly != nullptr; }
    size_t InteractionCount() const { return m_interactions.size(); }
    size_t PortalCount() const;
    size_t MovingPartCount() const { return m_movingTransforms.size(); }
    const asset::Sha256Digest* TopologySha256() const
    {
        return m_assembly ? &m_assembly->sourceManifestSha256 : nullptr;
    }

    AssemblyInteriorResult ActivateInteraction(uint32_t stableIndex);
    AssemblyInteriorResult ActivateInteraction(std::string_view id);
    AssemblyInteriorResult FindNearest(
        const AssemblyInteractionQuery& query) const;
    AssemblyInteriorResult ActivateNearest(
        const AssemblyInteractionQuery& query);
    AssemblyInteriorResult Advance(
        double dt,
        const AssemblyInteriorConfig& config = {});

    bool IsPortalTraversable(uint32_t stableIndex) const;
    uint32_t InteractionIndex(std::string_view id) const;
    uint32_t InteractionPortalIndex(uint32_t stableIndex) const;
    std::string_view InteractionStateName(uint32_t stableIndex) const;
    uint32_t InteractionStateIndex(uint32_t stableIndex) const;
    double InteractionMotionProgress(uint32_t stableIndex) const;
    const ecs::Transform* MovingPartLocalTransform(uint32_t stableIndex) const;

    AssemblyInteriorSnapshot CaptureSnapshot() const;
    AssemblyInteriorResult ApplySnapshot(
        const AssemblyInteriorSnapshot& snapshot);

private:
    struct InteractionRecord
    {
        uint32_t stateIndex = asset::kAssemblyNoIndex;
        double motionProgress = 0.0;
        uint32_t closedState = asset::kAssemblyNoIndex;
        uint32_t openingState = asset::kAssemblyNoIndex;
        uint32_t openState = asset::kAssemblyNoIndex;
        uint32_t closingState = asset::kAssemblyNoIndex;
        uint32_t lockedState = asset::kAssemblyNoIndex;
    };

    bool ValidateSnapshotRecord(
        uint32_t interactionIndex,
        const AssemblyInteractionSnapshot& snapshot) const;
    void RebuildMovingTransforms();

    std::shared_ptr<const asset::CookedAssembly> m_assembly;
    std::vector<PreparedAssemblyModule> m_modules;
    std::vector<PreparedAssemblyMovingPart> m_movingParts;
    std::vector<InteractionRecord> m_interactions;
    std::vector<ecs::Transform> m_movingTransforms;
    std::map<std::string, uint32_t, std::less<>> m_interactionIndices;
};

} // namespace scene
