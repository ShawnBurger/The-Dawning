#pragma once

#include "assembly_collision_runtime.h"
#include "assembly_interior_runtime.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace scene
{

enum class AssemblyDynamicCollisionStatus : uint8_t
{
    Success,
    InvalidArgument,
    NotInitialized,
    TopologyMismatch,
    InvalidTopology,
    ResourceLimitExceeded,
    CollisionFailure,
    AllocationFailure,
    InternalError
};

const char* AssemblyDynamicCollisionStatusName(
    AssemblyDynamicCollisionStatus status);

struct AssemblyDynamicCollisionResult
{
    AssemblyDynamicCollisionStatus status =
        AssemblyDynamicCollisionStatus::InvalidArgument;
    bool changed = false;
    std::string error;

    bool Succeeded() const
    {
        return status == AssemblyDynamicCollisionStatus::Success;
    }
};

struct AssemblyDynamicCollisionConfig
{
    double panelThicknessMeters = 0.12;
    uint64_t maxDynamicBoxes = 400'000ull;
    InteriorCollisionWorldLimits combinedWorldLimits;
};

struct AssemblyInteriorCollisionSnapshot final
{
    asset::Sha256Digest topologySha256;
    uint64_t revision = 0;
    std::shared_ptr<const InteriorCollisionWorld> collisionWorld;
    std::vector<InteriorCollisionBox> dynamicBoxes;
};

// Builds immutable assembly-local collision snapshots from the authored
// interaction state. The static world remains leased and unchanged; each
// successful state change publishes a complete validated combined world.
class AssemblyDynamicCollisionRuntime final
{
public:
    AssemblyDynamicCollisionRuntime() = default;
    AssemblyDynamicCollisionRuntime(
        const AssemblyDynamicCollisionRuntime&) = delete;
    AssemblyDynamicCollisionRuntime& operator=(
        const AssemblyDynamicCollisionRuntime&) = delete;

    AssemblyDynamicCollisionResult Initialize(
        std::shared_ptr<const asset::CookedAssembly> assembly,
        std::shared_ptr<const InteriorCollisionWorld> staticWorld,
        const AssemblyInteriorRuntime& interior,
        const AssemblyDynamicCollisionConfig& config = {});

    AssemblyDynamicCollisionResult Refresh(
        const AssemblyInteriorRuntime& interior);

    void Shutdown() noexcept;

    bool IsInitialized() const { return m_snapshot != nullptr; }
    const std::shared_ptr<const AssemblyInteriorCollisionSnapshot>& Snapshot()
        const { return m_snapshot; }
    bool IsCurrent(
        const std::shared_ptr<const AssemblyInteriorCollisionSnapshot>& snapshot)
        const { return snapshot && snapshot == m_snapshot; }

    // Used by the host only to complete a higher-level rollback without a
    // second allocation. The lease must come from this topology.
    bool RestorePublishedSnapshot(
        std::shared_ptr<const AssemblyInteriorCollisionSnapshot> snapshot)
        noexcept;

private:
    AssemblyDynamicCollisionResult BuildCandidate(
        const AssemblyInteriorRuntime& interior,
        uint64_t revision,
        std::shared_ptr<const AssemblyInteriorCollisionSnapshot>& candidate)
        const;

    std::shared_ptr<const asset::CookedAssembly> m_assembly;
    std::shared_ptr<const InteriorCollisionWorld> m_staticWorld;
    std::shared_ptr<const AssemblyInteriorCollisionSnapshot> m_snapshot;
    AssemblyDynamicCollisionConfig m_config;
};

} // namespace scene
