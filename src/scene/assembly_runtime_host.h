#pragma once

#include "assembly_collision_runtime.h"
#include "assembly_dynamic_collision_runtime.h"
#include "assembly_interior_runtime.h"
#include "assembly_runtime_resources.h"
#include "model_loader.h"
#include "../asset/runtime_content_manifest.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace scene
{

enum class AssemblyRuntimeHostStatus : uint8_t
{
    Success,
    InvalidState,
    ManifestFailure,
    AssemblyFailure,
    CoverageFailure,
    ModelFailure,
    CollisionFailure,
    ResourceFailure,
    CatalogFailure,
    PreparationFailure,
    CommitFailure,
    InteriorFailure,
    AllocationFailure,
    InternalError
};

const char* AssemblyRuntimeHostStatusName(AssemblyRuntimeHostStatus status);

struct AssemblyRuntimeHostResult
{
    AssemblyRuntimeHostStatus status = AssemblyRuntimeHostStatus::InvalidState;
    std::string error;

    bool Succeeded() const
    {
        return status == AssemblyRuntimeHostStatus::Success;
    }
};

// Owns one data-driven assembly from manifest load through explicit ECS and GPU
// resource teardown. BeginLoad records uploads on the caller's currently open
// command list. CommitAfterUploadRetirement must be called only after that list
// has executed and the GPU copy work has retired.
class AssemblyRuntimeHost final
{
public:
    AssemblyRuntimeHost() = default;
    AssemblyRuntimeHost(const AssemblyRuntimeHost&) = delete;
    AssemblyRuntimeHost& operator=(const AssemblyRuntimeHost&) = delete;

    AssemblyRuntimeHostResult BeginLoad(
        Scene& scene,
        render::D3D12Device& device,
        render::Renderer& renderer,
        const std::filesystem::path& manifestPath);

    AssemblyRuntimeHostResult CommitAfterUploadRetirement(Scene& scene);

    AssemblyInteriorResult ActivateInteraction(
        Scene& scene,
        uint32_t stableIndex);
    AssemblyInteriorResult ActivateInteraction(
        Scene& scene,
        std::string_view id);
    AssemblyInteriorResult ActivateNearestInteraction(
        Scene& scene,
        const AssemblyInteractionQuery& query);
    AssemblyInteriorResult AdvanceInterior(
        Scene& scene,
        double dt,
        const AssemblyInteriorConfig& config = {});
    AssemblyInteriorSnapshot CaptureInteriorSnapshot() const;
    AssemblyInteriorResult ApplyInteriorSnapshot(
        Scene& scene,
        const AssemblyInteriorSnapshot& snapshot);
    AssemblyInteriorResult SynchronizePresentation(Scene& scene);

    bool Shutdown(
        Scene& scene,
        render::D3D12Device& device,
        render::Renderer& renderer) noexcept;

    bool IsPending() const { return m_pending; }
    bool IsLive() const { return m_instance && m_instance->IsAlive(); }
    const AssemblyInstance* Instance() const { return m_instance.get(); }
    const AssemblyInteriorRuntime& Interior() const { return m_interior; }
    const InteriorCollisionWorld* CollisionWorld() const
    {
        return m_collisionWorld.get();
    }
    std::shared_ptr<const AssemblyInteriorCollisionSnapshot>
        InteractiveCollisionSnapshot() const
    {
        return m_dynamicCollision.Snapshot();
    }
    const asset::RuntimeContentManifest& Manifest() const { return m_manifest; }

private:
    AssemblyInteriorResult ValidateInteriorEntities(Scene& scene) const;
    AssemblyInteriorResult CaptureInteriorForMutation(
        AssemblyInteriorSnapshot& snapshot) const;
    AssemblyInteriorResult RefreshDynamicCollision();
    AssemblyInteriorResult RollbackInteriorMutation(
        const AssemblyInteriorSnapshot& interior,
        std::shared_ptr<const AssemblyInteriorCollisionSnapshot> collision,
        AssemblyInteriorResult failure);

    void ReleaseState(
        Scene& scene,
        render::D3D12Device& device,
        render::Renderer& renderer) noexcept;

    asset::RuntimeContentManifest m_manifest;
    std::shared_ptr<const asset::CookedAssembly> m_assembly;
    std::vector<LoadedModelResources> m_models;
    std::map<std::string, std::shared_ptr<const asset::CookedCollision>>
        m_collisions;
    std::shared_ptr<const InteriorCollisionWorld> m_collisionWorld;
    AssemblyDynamicCollisionRuntime m_dynamicCollision;
    std::shared_ptr<AssemblyRuntimeResourceOwners> m_owners;
    std::unique_ptr<asset::AssemblyResourceCatalogStore> m_catalog;
    std::shared_ptr<const PreparedAssemblyPlan> m_plan;
    std::shared_ptr<AssemblyInstance> m_instance;
    AssemblyInteriorRuntime m_interior;
    std::vector<ecs::Transform> m_presentationModuleWorld;
    std::vector<ecs::Transform> m_presentationMovingLocal;
    std::vector<ecs::Transform> m_presentationMovingWorld;
    bool m_pending = false;
};

} // namespace scene
