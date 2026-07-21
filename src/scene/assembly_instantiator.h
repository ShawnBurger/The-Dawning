#pragma once

#include "../asset/assembly_resource_catalog.h"
#include "../ecs/components.h"
#include "../ecs/entity.h"
#include "../ecs/registry.h"
#include "resource_handle.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace scene
{

enum class AssemblyRuntimeAdapterStatus : uint8_t
{
    Success,
    NotFound,
    Stale,
    Unsupported,
    InvalidResource,
    InternalError
};

const char* AssemblyRuntimeAdapterStatusName(AssemblyRuntimeAdapterStatus status);

struct PreparedAssemblyVisual
{
    MeshHandle mesh;
    ecs::Material material;
};

class AssemblyRuntimeResourceAdapter
{
public:
    virtual ~AssemblyRuntimeResourceAdapter() = default;

    virtual AssemblyRuntimeAdapterStatus PrepareVisual(
        const asset::AssemblyVisualIdentity& identity,
        uint64_t ownerToken,
        PreparedAssemblyVisual& visual) const = 0;

    virtual AssemblyRuntimeAdapterStatus ValidateCollision(
        const asset::AssemblyCollisionIdentity& identity,
        uint64_t ownerToken) const = 0;

    virtual AssemblyRuntimeAdapterStatus ValidateNavigationMesh(
        const asset::AssemblyNavigationMeshIdentity& identity,
        uint64_t ownerToken) const = 0;

    virtual AssemblyRuntimeAdapterStatus ValidateWalkableSurface(
        const asset::AssemblyWalkableSurfaceIdentity& identity,
        uint64_t ownerToken) const = 0;
};

struct PreparedAssemblyBinding
{
    asset::AssemblyCatalogIdentity identity;
    uint64_t ownerToken = 0;
};

struct PreparedAssemblyVisualBinding
{
    PreparedAssemblyBinding catalog;
    PreparedAssemblyVisual runtime;
};

struct PreparedAssemblyModule
{
    uint32_t stableIndex = 0;
    // Immutable pose authored relative to the assembly root. Runtime
    // presentation composes this through the live root each fixed step.
    ecs::Transform localTransform;
    PreparedAssemblyVisualBinding visual;
    PreparedAssemblyBinding collision;
    std::vector<PreparedAssemblyVisualBinding> lods;
};

struct PreparedAssemblyZone
{
    uint32_t stableIndex = 0;
    PreparedAssemblyBinding navigationMesh;
    PreparedAssemblyBinding walkableSurface;
};

struct PreparedAssemblyMovingPart
{
    uint32_t stableIndex = 0;
    uint32_t moduleIndex = asset::kAssemblyNoIndex;
    uint32_t interactionIndex = asset::kAssemblyNoIndex;
    // Closed pose in assembly-local space. The interior runtime owns the
    // current local pose; presentation alone converts it to world space.
    ecs::Transform localTransform;
    PreparedAssemblyVisualBinding visual;
};

struct AssemblyInstantiationLimits
{
    uint64_t maxEntities = 250'000ull;
    uint64_t maxBindings = 1'000'000ull;
    uint32_t maxLodsPerModule = 32u;
    asset::AssemblyResourceResolutionLimits resolution;
};

enum class AssemblyInstantiationStatus : uint8_t
{
    Success,
    InvalidArgument,
    InvalidResolvedResources,
    ResourceLimitExceeded,
    CatalogLeaseMismatch,
    AdapterRejected,
    InvalidRuntimeBinding,
    EntityCapacityExceeded,
    AllocationFailure,
    InternalError
};

const char* AssemblyInstantiationStatusName(AssemblyInstantiationStatus status);

class PreparedAssemblyPlan
{
public:
    const std::shared_ptr<const asset::ResolvedAssemblyResources>& Resources() const;
    const std::shared_ptr<const asset::AssemblyResourceCatalogSnapshot>&
        CatalogLease() const;
    const ecs::Transform& RootTransform() const;
    std::span<const PreparedAssemblyModule> Modules() const;
    std::span<const PreparedAssemblyZone> Zones() const;
    std::span<const PreparedAssemblyMovingPart> MovingParts() const;
    uint64_t EntityCount() const;
    uint64_t BindingCount() const;

private:
    PreparedAssemblyPlan() = default;

    std::shared_ptr<const asset::ResolvedAssemblyResources> m_resources;
    std::shared_ptr<const asset::AssemblyResourceCatalogSnapshot> m_catalogLease;
    ecs::Transform m_rootTransform;
    std::vector<PreparedAssemblyModule> m_modules;
    std::vector<PreparedAssemblyZone> m_zones;
    std::vector<PreparedAssemblyMovingPart> m_movingParts;
    uint64_t m_entityCount = 0;
    uint64_t m_bindingCount = 0;

    friend struct AssemblyPreparationResult;
    friend AssemblyPreparationResult PrepareAssemblyInstance(
        std::shared_ptr<const asset::CookedAssembly>,
        std::shared_ptr<const asset::AssemblyResourceCatalogSnapshot>,
        const AssemblyRuntimeResourceAdapter&,
        const ecs::Transform&,
        const AssemblyInstantiationLimits&);
};

struct AssemblyPreparationResult
{
    AssemblyInstantiationStatus status =
        AssemblyInstantiationStatus::InvalidArgument;
    std::shared_ptr<const PreparedAssemblyPlan> plan;
    asset::AssemblyResourceKind failedKind = asset::AssemblyResourceKind::Visual;
    uint32_t failedStableIndex = asset::kAssemblyNoIndex;
    AssemblyRuntimeAdapterStatus adapterStatus =
        AssemblyRuntimeAdapterStatus::Success;
    asset::AssemblyResourceResolutionStatus resolutionStatus =
        asset::AssemblyResourceResolutionStatus::Success;

    bool Succeeded() const
    {
        return status == AssemblyInstantiationStatus::Success && plan != nullptr;
    }
};

AssemblyPreparationResult PrepareAssemblyInstance(
    std::shared_ptr<const asset::CookedAssembly> assembly,
    std::shared_ptr<const asset::AssemblyResourceCatalogSnapshot> catalogLease,
    const AssemblyRuntimeResourceAdapter& adapter,
    const ecs::Transform& rootTransform = {},
    const AssemblyInstantiationLimits& limits = {});

class AssemblyEntityTarget
{
public:
    virtual ~AssemblyEntityTarget() = default;

    virtual ecs::Entity CreateEntity() = 0;
    virtual void DestroyEntity(ecs::Entity entity) noexcept = 0;
    virtual bool IsAlive(ecs::Entity entity) const noexcept = 0;
    virtual const void* TargetIdentity() const noexcept = 0;
    virtual void AssignTransform(
        ecs::Entity entity,
        const ecs::Transform& transform) = 0;
    virtual void AssignMeshInstance(
        ecs::Entity entity,
        const ecs::MeshInstance& mesh) = 0;
    virtual void AssignMaterial(
        ecs::Entity entity,
        const ecs::Material& material) = 0;
    virtual void AssignName(ecs::Entity entity, const ecs::Name& name) = 0;
    virtual void AssignParent(ecs::Entity entity, const ecs::Parent& parent) = 0;
};

class RegistryAssemblyEntityTarget final : public AssemblyEntityTarget
{
public:
    explicit RegistryAssemblyEntityTarget(ecs::Registry& registry);

    ecs::Entity CreateEntity() override;
    void DestroyEntity(ecs::Entity entity) noexcept override;
    bool IsAlive(ecs::Entity entity) const noexcept override;
    const void* TargetIdentity() const noexcept override;
    void AssignTransform(
        ecs::Entity entity,
        const ecs::Transform& transform) override;
    void AssignMeshInstance(
        ecs::Entity entity,
        const ecs::MeshInstance& mesh) override;
    void AssignMaterial(
        ecs::Entity entity,
        const ecs::Material& material) override;
    void AssignName(ecs::Entity entity, const ecs::Name& name) override;
    void AssignParent(ecs::Entity entity, const ecs::Parent& parent) override;

private:
    ecs::Registry& m_registry;
};

class AssemblyInstance
{
public:
    bool IsAlive() const;
    const std::shared_ptr<const PreparedAssemblyPlan>& Plan() const;
    ecs::Entity RootEntity() const;
    std::span<const ecs::Entity> ModuleEntities() const;
    std::span<const ecs::Entity> MovingPartEntities() const;

private:
    std::shared_ptr<const PreparedAssemblyPlan> m_plan;
    ecs::Entity m_rootEntity;
    std::vector<ecs::Entity> m_moduleEntities;
    std::vector<ecs::Entity> m_movingPartEntities;
    const void* m_targetIdentity = nullptr;
    bool m_alive = false;

    friend struct AssemblyCommitResult;
    friend AssemblyCommitResult CommitPreparedAssembly(
        AssemblyEntityTarget&,
        std::shared_ptr<const PreparedAssemblyPlan>);
    friend AssemblyInstantiationStatus DestroyAssemblyInstance(
        AssemblyEntityTarget&,
        AssemblyInstance&);
};

struct AssemblyCommitResult
{
    AssemblyInstantiationStatus status =
        AssemblyInstantiationStatus::InvalidArgument;
    std::shared_ptr<AssemblyInstance> instance;
    uint64_t stagedEntityCount = 0;

    bool Succeeded() const
    {
        return status == AssemblyInstantiationStatus::Success &&
               instance != nullptr && instance->IsAlive();
    }
};

AssemblyCommitResult CommitPreparedAssembly(
    AssemblyEntityTarget& target,
    std::shared_ptr<const PreparedAssemblyPlan> plan);

AssemblyInstantiationStatus DestroyAssemblyInstance(
    AssemblyEntityTarget& target,
    AssemblyInstance& instance);

} // namespace scene
