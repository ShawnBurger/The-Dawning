#include "assembly_instantiator.h"
#include "assembly_presentation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace scene
{
namespace
{

constexpr double kDegreesToRadians = 0.017453292519943295769;

template <asset::AssemblyResourceKind Kind>
asset::AssemblyCatalogIdentity GenericIdentity(
    const asset::TypedAssemblyResourceIdentity<Kind>& identity)
{
    asset::AssemblyCatalogIdentity result;
    result.kind = Kind;
    result.value = identity.value;
    result.generation = identity.generation;
    result.contentSha256 = identity.contentSha256;
    return result;
}

bool Finite(float value)
{
    return std::isfinite(value);
}

bool Finite(double value)
{
    return std::isfinite(value);
}

bool ValidTransform(const ecs::Transform& transform)
{
    if (!Finite(transform.position.x) || !Finite(transform.position.y) ||
        !Finite(transform.position.z) || !Finite(transform.rotation.x) ||
        !Finite(transform.rotation.y) || !Finite(transform.rotation.z) ||
        !Finite(transform.rotation.w) || !Finite(transform.scale.x) ||
        !Finite(transform.scale.y) || !Finite(transform.scale.z) ||
        transform.scale.x <= 0.0f || transform.scale.y <= 0.0f ||
        transform.scale.z <= 0.0f)
    {
        return false;
    }

    const double rotationLengthSquared =
        static_cast<double>(transform.rotation.x) * transform.rotation.x +
        static_cast<double>(transform.rotation.y) * transform.rotation.y +
        static_cast<double>(transform.rotation.z) * transform.rotation.z +
        static_cast<double>(transform.rotation.w) * transform.rotation.w;
    return Finite(rotationLengthSquared) && rotationLengthSquared > 1.0e-12;
}

bool ValidMaterial(const ecs::Material& material)
{
    const auto finiteColor = [](const core::Color& color) {
        return Finite(color.r) && Finite(color.g) && Finite(color.b) &&
               Finite(color.a);
    };
    return finiteColor(material.albedo) && finiteColor(material.emissive) &&
           Finite(material.roughness) && material.roughness >= 0.0f &&
           material.roughness <= 1.0f && Finite(material.metallic) &&
           material.metallic >= 0.0f && material.metallic <= 1.0f &&
           Finite(material.emissiveStrength) &&
           material.emissiveStrength >= 0.0f;
}

bool AddWithinLimit(uint64_t value, uint64_t limit, uint64_t& total)
{
    if (value > limit || total > limit - value)
        return false;
    total += value;
    return true;
}

bool ComposeTransform(
    const ecs::Transform& root,
    const asset::AssemblyTransform& local,
    ecs::Transform& world)
{
    for (const double value : local.positionMeters)
    {
        if (!Finite(value))
            return false;
    }
    for (const double value : local.rotationEulerDegrees)
    {
        if (!Finite(value))
            return false;
    }
    for (const double value : local.scale)
    {
        if (!Finite(value) || value <= 0.0 ||
            value > (std::numeric_limits<float>::max)())
        {
            return false;
        }
    }

    const core::Quatf rootRotation = root.rotation.Normalized();
    const float pitch = static_cast<float>(
        std::remainder(local.rotationEulerDegrees[0], 360.0) *
        kDegreesToRadians);
    const float yaw = static_cast<float>(
        std::remainder(local.rotationEulerDegrees[1], 360.0) *
        kDegreesToRadians);
    const float roll = static_cast<float>(
        std::remainder(local.rotationEulerDegrees[2], 360.0) *
        kDegreesToRadians);
    const core::Quatf localRotation = core::Quatf::FromEuler(pitch, yaw, roll);

    const double scaledX = local.positionMeters[0] * root.scale.x;
    const double scaledY = local.positionMeters[1] * root.scale.y;
    const double scaledZ = local.positionMeters[2] * root.scale.z;
    constexpr double maxFloat = (std::numeric_limits<float>::max)();
    if (!Finite(scaledX) || !Finite(scaledY) || !Finite(scaledZ) ||
        std::abs(scaledX) > maxFloat || std::abs(scaledY) > maxFloat ||
        std::abs(scaledZ) > maxFloat)
    {
        return false;
    }

    const core::Vec3f rotated = rootRotation.Rotate(core::Vec3f{
        static_cast<float>(scaledX),
        static_cast<float>(scaledY),
        static_cast<float>(scaledZ)
    });
    world.position = {
        root.position.x + static_cast<double>(rotated.x),
        root.position.y + static_cast<double>(rotated.y),
        root.position.z + static_cast<double>(rotated.z)
    };
    world.rotation = (rootRotation * localRotation).Normalized();

    const double scaleX = static_cast<double>(root.scale.x) * local.scale[0];
    const double scaleY = static_cast<double>(root.scale.y) * local.scale[1];
    const double scaleZ = static_cast<double>(root.scale.z) * local.scale[2];
    if (!Finite(scaleX) || !Finite(scaleY) || !Finite(scaleZ) ||
        scaleX <= 0.0 || scaleY <= 0.0 || scaleZ <= 0.0 ||
        scaleX > maxFloat || scaleY > maxFloat || scaleZ > maxFloat)
    {
        return false;
    }
    world.scale = {
        static_cast<float>(scaleX),
        static_cast<float>(scaleY),
        static_cast<float>(scaleZ)
    };
    return ValidTransform(world);
}

AssemblyPreparationResult PreparationFailure(
    AssemblyInstantiationStatus status,
    asset::AssemblyResourceKind kind = asset::AssemblyResourceKind::Visual,
    uint32_t stableIndex = asset::kAssemblyNoIndex,
    AssemblyRuntimeAdapterStatus adapterStatus =
        AssemblyRuntimeAdapterStatus::Success,
    asset::AssemblyResourceResolutionStatus resolutionStatus =
        asset::AssemblyResourceResolutionStatus::Success)
{
    AssemblyPreparationResult result;
    result.status = status;
    result.failedKind = kind;
    result.failedStableIndex = stableIndex;
    result.adapterStatus = adapterStatus;
    result.resolutionStatus = resolutionStatus;
    return result;
}

bool IsKnownAdapterStatus(AssemblyRuntimeAdapterStatus status)
{
    switch (status)
    {
    case AssemblyRuntimeAdapterStatus::Success:
    case AssemblyRuntimeAdapterStatus::NotFound:
    case AssemblyRuntimeAdapterStatus::Stale:
    case AssemblyRuntimeAdapterStatus::Unsupported:
    case AssemblyRuntimeAdapterStatus::InvalidResource:
    case AssemblyRuntimeAdapterStatus::InternalError:
        return true;
    }
    return false;
}

template <asset::AssemblyResourceKind Kind>
AssemblyInstantiationStatus ResolveLeaseBinding(
    const asset::AssemblyResourceCatalogSnapshot& lease,
    const asset::TypedAssemblyResourceIdentity<Kind>& identity,
    PreparedAssemblyBinding& binding)
{
    binding = {};
    binding.identity = GenericIdentity(identity);
    if (!identity.IsValid() || identity.generation == 0 ||
        !lease.TryGetOwnerToken(binding.identity, binding.ownerToken) ||
        binding.ownerToken == 0)
    {
        binding = {};
        return AssemblyInstantiationStatus::CatalogLeaseMismatch;
    }
    return AssemblyInstantiationStatus::Success;
}

AssemblyInstantiationStatus PrepareVisualBinding(
    const asset::AssemblyResourceCatalogSnapshot& lease,
    const AssemblyRuntimeResourceAdapter& adapter,
    const asset::AssemblyVisualIdentity& identity,
    PreparedAssemblyVisualBinding& binding,
    AssemblyRuntimeAdapterStatus& adapterStatus)
{
    const AssemblyInstantiationStatus leaseStatus =
        ResolveLeaseBinding(lease, identity, binding.catalog);
    if (leaseStatus != AssemblyInstantiationStatus::Success)
        return leaseStatus;

    binding.runtime = {};
    adapterStatus = adapter.PrepareVisual(
        identity, binding.catalog.ownerToken, binding.runtime);
    if (!IsKnownAdapterStatus(adapterStatus))
        return AssemblyInstantiationStatus::InternalError;
    if (adapterStatus != AssemblyRuntimeAdapterStatus::Success)
        return AssemblyInstantiationStatus::AdapterRejected;
    if (!binding.runtime.mesh.IsValid() ||
        !ValidMaterial(binding.runtime.material))
    {
        return AssemblyInstantiationStatus::InvalidRuntimeBinding;
    }
    return AssemblyInstantiationStatus::Success;
}

template <asset::AssemblyResourceKind Kind, typename Validate>
AssemblyInstantiationStatus PrepareOpaqueBinding(
    const asset::AssemblyResourceCatalogSnapshot& lease,
    const asset::TypedAssemblyResourceIdentity<Kind>& identity,
    PreparedAssemblyBinding& binding,
    AssemblyRuntimeAdapterStatus& adapterStatus,
    Validate&& validate)
{
    const AssemblyInstantiationStatus leaseStatus =
        ResolveLeaseBinding(lease, identity, binding);
    if (leaseStatus != AssemblyInstantiationStatus::Success)
        return leaseStatus;

    adapterStatus = validate(identity, binding.ownerToken);
    if (!IsKnownAdapterStatus(adapterStatus))
        return AssemblyInstantiationStatus::InternalError;
    if (adapterStatus != AssemblyRuntimeAdapterStatus::Success)
        return AssemblyInstantiationStatus::AdapterRejected;
    return AssemblyInstantiationStatus::Success;
}

void RollbackEntities(
    AssemblyEntityTarget& target,
    std::span<const ecs::Entity> entities) noexcept
{
    for (auto it = entities.rbegin(); it != entities.rend(); ++it)
        target.DestroyEntity(*it);
}

ecs::Name MakeName(const std::string& value)
{
    ecs::Name name;
    name.Set(value.c_str());
    return name;
}

} // namespace

const char* AssemblyRuntimeAdapterStatusName(AssemblyRuntimeAdapterStatus status)
{
    switch (status)
    {
    case AssemblyRuntimeAdapterStatus::Success: return "success";
    case AssemblyRuntimeAdapterStatus::NotFound: return "not found";
    case AssemblyRuntimeAdapterStatus::Stale: return "stale";
    case AssemblyRuntimeAdapterStatus::Unsupported: return "unsupported";
    case AssemblyRuntimeAdapterStatus::InvalidResource: return "invalid resource";
    case AssemblyRuntimeAdapterStatus::InternalError: return "internal error";
    }
    return "unknown";
}

const char* AssemblyInstantiationStatusName(AssemblyInstantiationStatus status)
{
    switch (status)
    {
    case AssemblyInstantiationStatus::Success: return "success";
    case AssemblyInstantiationStatus::InvalidArgument: return "invalid argument";
    case AssemblyInstantiationStatus::InvalidResolvedResources: return "invalid resolved resources";
    case AssemblyInstantiationStatus::ResourceLimitExceeded: return "resource limit exceeded";
    case AssemblyInstantiationStatus::CatalogLeaseMismatch: return "catalog lease mismatch";
    case AssemblyInstantiationStatus::AdapterRejected: return "adapter rejected";
    case AssemblyInstantiationStatus::InvalidRuntimeBinding: return "invalid runtime binding";
    case AssemblyInstantiationStatus::EntityCapacityExceeded: return "entity capacity exceeded";
    case AssemblyInstantiationStatus::AllocationFailure: return "allocation failure";
    case AssemblyInstantiationStatus::InternalError: return "internal error";
    }
    return "unknown";
}

const std::shared_ptr<const asset::ResolvedAssemblyResources>&
PreparedAssemblyPlan::Resources() const
{
    return m_resources;
}

const std::shared_ptr<const asset::AssemblyResourceCatalogSnapshot>&
PreparedAssemblyPlan::CatalogLease() const
{
    return m_catalogLease;
}

const ecs::Transform& PreparedAssemblyPlan::RootTransform() const
{
    return m_rootTransform;
}

std::span<const PreparedAssemblyModule> PreparedAssemblyPlan::Modules() const
{
    return m_modules;
}

std::span<const PreparedAssemblyZone> PreparedAssemblyPlan::Zones() const
{
    return m_zones;
}

std::span<const PreparedAssemblyMovingPart>
PreparedAssemblyPlan::MovingParts() const
{
    return m_movingParts;
}

uint64_t PreparedAssemblyPlan::EntityCount() const
{
    return m_entityCount;
}

uint64_t PreparedAssemblyPlan::BindingCount() const
{
    return m_bindingCount;
}

AssemblyPreparationResult PrepareAssemblyInstance(
    std::shared_ptr<const asset::CookedAssembly> assemblySource,
    std::shared_ptr<const asset::AssemblyResourceCatalogSnapshot> catalogLease,
    const AssemblyRuntimeResourceAdapter& adapter,
    const ecs::Transform& rootTransform,
    const AssemblyInstantiationLimits& limits)
{
    if (!assemblySource || !catalogLease ||
        limits.maxEntities == 0 || limits.maxBindings == 0 ||
        limits.maxLodsPerModule == 0 || !ValidTransform(rootTransform))
    {
        return PreparationFailure(AssemblyInstantiationStatus::InvalidArgument);
    }

    const asset::CookedAssembly& sourceAssembly = *assemblySource;
    if (sourceAssembly.modules.size() >
            (std::numeric_limits<uint32_t>::max)() ||
        sourceAssembly.zones.size() > (std::numeric_limits<uint32_t>::max)() ||
        sourceAssembly.movingParts.size() >
            (std::numeric_limits<uint32_t>::max)())
    {
        return PreparationFailure(
            AssemblyInstantiationStatus::ResourceLimitExceeded);
    }

    uint64_t entityCount = 1;
    if (!AddWithinLimit(
            sourceAssembly.modules.size(), limits.maxEntities, entityCount) ||
        !AddWithinLimit(
            sourceAssembly.movingParts.size(), limits.maxEntities, entityCount))
    {
        return PreparationFailure(
            AssemblyInstantiationStatus::ResourceLimitExceeded);
    }

    uint64_t bindingCount = 0;
    for (size_t i = 0; i < sourceAssembly.modules.size(); ++i)
    {
        if (sourceAssembly.modules[i].lods.size() > limits.maxLodsPerModule ||
            !AddWithinLimit(
                2u + sourceAssembly.modules[i].lods.size(),
                limits.maxBindings,
                bindingCount))
        {
            return PreparationFailure(
                AssemblyInstantiationStatus::ResourceLimitExceeded,
                asset::AssemblyResourceKind::Visual,
                static_cast<uint32_t>(i));
        }
    }
    if (!AddWithinLimit(
            sourceAssembly.zones.size() * 2ull,
            limits.maxBindings,
            bindingCount) ||
        !AddWithinLimit(
            sourceAssembly.movingParts.size(),
            limits.maxBindings,
            bindingCount))
    {
        return PreparationFailure(
            AssemblyInstantiationStatus::ResourceLimitExceeded);
    }

    const asset::AssemblyResourceResolutionResult resolution =
        asset::ResolveAssemblyResources(
            assemblySource, *catalogLease, limits.resolution);
    if (!resolution.Succeeded())
    {
        AssemblyInstantiationStatus status =
            AssemblyInstantiationStatus::InvalidResolvedResources;
        switch (resolution.status)
        {
        case asset::AssemblyResourceResolutionStatus::ResourceLimitExceeded:
            status = AssemblyInstantiationStatus::ResourceLimitExceeded;
            break;
        case asset::AssemblyResourceResolutionStatus::CatalogNotFound:
        case asset::AssemblyResourceResolutionStatus::CatalogStale:
        case asset::AssemblyResourceResolutionStatus::CatalogError:
        case asset::AssemblyResourceResolutionStatus::InvalidCatalogIdentity:
        case asset::AssemblyResourceResolutionStatus::CatalogKindMismatch:
        case asset::AssemblyResourceResolutionStatus::IdentityConflict:
            status = AssemblyInstantiationStatus::CatalogLeaseMismatch;
            break;
        case asset::AssemblyResourceResolutionStatus::AllocationFailure:
            status = AssemblyInstantiationStatus::AllocationFailure;
            break;
        case asset::AssemblyResourceResolutionStatus::InternalError:
            status = AssemblyInstantiationStatus::InternalError;
            break;
        case asset::AssemblyResourceResolutionStatus::Success:
        case asset::AssemblyResourceResolutionStatus::InvalidArgument:
        case asset::AssemblyResourceResolutionStatus::InvalidAssembly:
            break;
        }
        return PreparationFailure(
            status,
            asset::AssemblyResourceKind::Visual,
            asset::kAssemblyNoIndex,
            AssemblyRuntimeAdapterStatus::Success,
            resolution.status);
    }
    std::shared_ptr<const asset::ResolvedAssemblyResources> resources =
        resolution.resources;
    const asset::CookedAssembly& assembly = *resources->assembly;
    if (resources->sourceManifestSha256 != assembly.sourceManifestSha256 ||
        resources->modules.size() != assembly.modules.size() ||
        resources->zones.size() != assembly.zones.size() ||
        resources->movingParts.size() != assembly.movingParts.size())
    {
        return PreparationFailure(
            AssemblyInstantiationStatus::InvalidResolvedResources);
    }

    for (size_t i = 0; i < assembly.modules.size(); ++i)
    {
        if (resources->modules[i].lods.size() != assembly.modules[i].lods.size())
        {
            return PreparationFailure(
                AssemblyInstantiationStatus::InvalidResolvedResources,
                asset::AssemblyResourceKind::Visual,
                static_cast<uint32_t>(i));
        }
    }

    try
    {
        std::shared_ptr<PreparedAssemblyPlan> plan(new PreparedAssemblyPlan());
        plan->m_resources = std::move(resources);
        plan->m_catalogLease = std::move(catalogLease);
        plan->m_rootTransform = rootTransform;
        plan->m_rootTransform.rotation = rootTransform.rotation.Normalized();
        plan->m_entityCount = entityCount;
        plan->m_bindingCount = bindingCount;
        plan->m_modules.reserve(assembly.modules.size());
        plan->m_zones.reserve(assembly.zones.size());
        plan->m_movingParts.reserve(assembly.movingParts.size());

        for (size_t i = 0; i < assembly.modules.size(); ++i)
        {
            PreparedAssemblyModule prepared;
            prepared.stableIndex = static_cast<uint32_t>(i);
            ecs::Transform validatedWorld;
            if (!ComposeTransform(
                    ecs::Transform{},
                    assembly.modules[i].transform,
                    prepared.localTransform) ||
                !ComposeTransform(
                    plan->m_rootTransform,
                    assembly.modules[i].transform,
                    validatedWorld))
            {
                return PreparationFailure(
                    AssemblyInstantiationStatus::InvalidResolvedResources,
                    asset::AssemblyResourceKind::Visual,
                    prepared.stableIndex);
            }

            AssemblyRuntimeAdapterStatus adapterStatus =
                AssemblyRuntimeAdapterStatus::Success;
            AssemblyInstantiationStatus status = PrepareVisualBinding(
                *plan->m_catalogLease,
                adapter,
                plan->m_resources->modules[i].visual,
                prepared.visual,
                adapterStatus);
            if (status != AssemblyInstantiationStatus::Success)
            {
                return PreparationFailure(
                    status,
                    asset::AssemblyResourceKind::Visual,
                    prepared.stableIndex,
                    adapterStatus);
            }

            status = PrepareOpaqueBinding(
                *plan->m_catalogLease,
                plan->m_resources->modules[i].collision,
                prepared.collision,
                adapterStatus,
                [&](const asset::AssemblyCollisionIdentity& identity,
                    uint64_t ownerToken) {
                    return adapter.ValidateCollision(identity, ownerToken);
                });
            if (status != AssemblyInstantiationStatus::Success)
            {
                return PreparationFailure(
                    status,
                    asset::AssemblyResourceKind::Collision,
                    prepared.stableIndex,
                    adapterStatus);
            }

            prepared.lods.reserve(plan->m_resources->modules[i].lods.size());
            for (const asset::AssemblyVisualIdentity& lod :
                 plan->m_resources->modules[i].lods)
            {
                PreparedAssemblyVisualBinding preparedLod;
                status = PrepareVisualBinding(
                    *plan->m_catalogLease,
                    adapter,
                    lod,
                    preparedLod,
                    adapterStatus);
                if (status != AssemblyInstantiationStatus::Success)
                {
                    return PreparationFailure(
                        status,
                        asset::AssemblyResourceKind::Visual,
                        prepared.stableIndex,
                        adapterStatus);
                }
                prepared.lods.push_back(std::move(preparedLod));
            }
            plan->m_modules.push_back(std::move(prepared));
        }

        for (size_t i = 0; i < assembly.zones.size(); ++i)
        {
            if (assembly.zones[i].moduleIndex >= assembly.modules.size())
            {
                return PreparationFailure(
                    AssemblyInstantiationStatus::InvalidResolvedResources,
                    asset::AssemblyResourceKind::NavigationMesh,
                    static_cast<uint32_t>(i));
            }

            PreparedAssemblyZone prepared;
            prepared.stableIndex = static_cast<uint32_t>(i);
            AssemblyRuntimeAdapterStatus adapterStatus =
                AssemblyRuntimeAdapterStatus::Success;
            AssemblyInstantiationStatus status = PrepareOpaqueBinding(
                *plan->m_catalogLease,
                plan->m_resources->zones[i].navigationMesh,
                prepared.navigationMesh,
                adapterStatus,
                [&](const asset::AssemblyNavigationMeshIdentity& identity,
                    uint64_t ownerToken) {
                    return adapter.ValidateNavigationMesh(identity, ownerToken);
                });
            if (status != AssemblyInstantiationStatus::Success)
            {
                return PreparationFailure(
                    status,
                    asset::AssemblyResourceKind::NavigationMesh,
                    prepared.stableIndex,
                    adapterStatus);
            }

            status = PrepareOpaqueBinding(
                *plan->m_catalogLease,
                plan->m_resources->zones[i].walkableSurface,
                prepared.walkableSurface,
                adapterStatus,
                [&](const asset::AssemblyWalkableSurfaceIdentity& identity,
                    uint64_t ownerToken) {
                    return adapter.ValidateWalkableSurface(identity, ownerToken);
                });
            if (status != AssemblyInstantiationStatus::Success)
            {
                return PreparationFailure(
                    status,
                    asset::AssemblyResourceKind::WalkableSurface,
                    prepared.stableIndex,
                    adapterStatus);
            }
            plan->m_zones.push_back(std::move(prepared));
        }

        for (size_t i = 0; i < assembly.movingParts.size(); ++i)
        {
            const asset::AssemblyMovingPart& source = assembly.movingParts[i];
            if (source.moduleIndex >= assembly.modules.size() ||
                source.interactionIndex >= assembly.interactions.size())
            {
                return PreparationFailure(
                    AssemblyInstantiationStatus::InvalidResolvedResources,
                    asset::AssemblyResourceKind::Visual,
                    static_cast<uint32_t>(i));
            }

            PreparedAssemblyMovingPart prepared;
            prepared.stableIndex = static_cast<uint32_t>(i);
            prepared.moduleIndex = source.moduleIndex;
            prepared.interactionIndex = source.interactionIndex;
            prepared.localTransform =
                plan->m_modules[source.moduleIndex].localTransform;
            AssemblyRuntimeAdapterStatus adapterStatus =
                AssemblyRuntimeAdapterStatus::Success;
            const AssemblyInstantiationStatus status = PrepareVisualBinding(
                *plan->m_catalogLease,
                adapter,
                plan->m_resources->movingParts[i].visual,
                prepared.visual,
                adapterStatus);
            if (status != AssemblyInstantiationStatus::Success)
            {
                return PreparationFailure(
                    status,
                    asset::AssemblyResourceKind::Visual,
                    prepared.stableIndex,
                    adapterStatus);
            }
            plan->m_movingParts.push_back(std::move(prepared));
        }

        AssemblyPreparationResult result;
        result.status = AssemblyInstantiationStatus::Success;
        result.plan = std::move(plan);
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return PreparationFailure(AssemblyInstantiationStatus::AllocationFailure);
    }
    catch (...)
    {
        return PreparationFailure(AssemblyInstantiationStatus::InternalError);
    }
}

RegistryAssemblyEntityTarget::RegistryAssemblyEntityTarget(ecs::Registry& registry)
    : m_registry(registry)
{
}

ecs::Entity RegistryAssemblyEntityTarget::CreateEntity()
{
    return m_registry.Create();
}

void RegistryAssemblyEntityTarget::DestroyEntity(ecs::Entity entity) noexcept
{
    m_registry.Destroy(entity);
}

bool RegistryAssemblyEntityTarget::IsAlive(ecs::Entity entity) const noexcept
{
    return m_registry.IsAlive(entity);
}

const void* RegistryAssemblyEntityTarget::TargetIdentity() const noexcept
{
    return &m_registry;
}

void RegistryAssemblyEntityTarget::AssignTransform(
    ecs::Entity entity,
    const ecs::Transform& transform)
{
    m_registry.Assign<ecs::Transform>(entity, transform);
}

void RegistryAssemblyEntityTarget::AssignMeshInstance(
    ecs::Entity entity,
    const ecs::MeshInstance& mesh)
{
    m_registry.Assign<ecs::MeshInstance>(entity, mesh);
}

void RegistryAssemblyEntityTarget::AssignMaterial(
    ecs::Entity entity,
    const ecs::Material& material)
{
    m_registry.Assign<ecs::Material>(entity, material);
}

void RegistryAssemblyEntityTarget::AssignName(
    ecs::Entity entity,
    const ecs::Name& name)
{
    m_registry.Assign<ecs::Name>(entity, name);
}

void RegistryAssemblyEntityTarget::AssignParent(
    ecs::Entity entity,
    const ecs::Parent& parent)
{
    m_registry.Assign<ecs::Parent>(entity, parent);
}

bool AssemblyInstance::IsAlive() const
{
    return m_alive;
}

const std::shared_ptr<const PreparedAssemblyPlan>& AssemblyInstance::Plan() const
{
    return m_plan;
}

ecs::Entity AssemblyInstance::RootEntity() const
{
    return m_rootEntity;
}

std::span<const ecs::Entity> AssemblyInstance::ModuleEntities() const
{
    return m_moduleEntities;
}

std::span<const ecs::Entity> AssemblyInstance::MovingPartEntities() const
{
    return m_movingPartEntities;
}

AssemblyCommitResult CommitPreparedAssembly(
    AssemblyEntityTarget& target,
    std::shared_ptr<const PreparedAssemblyPlan> plan)
{
    AssemblyCommitResult result;
    if (!plan || !plan->Resources() || !plan->Resources()->assembly ||
        !plan->CatalogLease() ||
        target.TargetIdentity() == nullptr ||
        plan->Modules().size() != plan->Resources()->assembly->modules.size() ||
        plan->MovingParts().size() !=
            plan->Resources()->assembly->movingParts.size() ||
        plan->EntityCount() !=
            1ull + plan->Modules().size() + plan->MovingParts().size())
    {
        result.status = AssemblyInstantiationStatus::InvalidArgument;
        return result;
    }

    std::vector<ecs::Entity> staged;
    try
    {
        std::shared_ptr<AssemblyInstance> instance(new AssemblyInstance());
        instance->m_plan = std::move(plan);
        instance->m_targetIdentity = target.TargetIdentity();
        staged.reserve(static_cast<size_t>(instance->m_plan->EntityCount()));
        instance->m_moduleEntities.reserve(instance->m_plan->Modules().size());
        instance->m_movingPartEntities.reserve(
            instance->m_plan->MovingParts().size());

        std::vector<ecs::Transform> movingLocalTransforms;
        std::vector<ecs::Transform> moduleWorldTransforms(
            instance->m_plan->Modules().size());
        std::vector<ecs::Transform> movingPartWorldTransforms(
            instance->m_plan->MovingParts().size());
        movingLocalTransforms.reserve(instance->m_plan->MovingParts().size());
        for (const PreparedAssemblyMovingPart& part :
             instance->m_plan->MovingParts())
        {
            movingLocalTransforms.push_back(part.localTransform);
        }
        AssemblyPresentationConfig presentationConfig;
        // Generic assembly publication preserves the existing support for
        // nonuniform authored roots. The playable-ship host applies the stricter
        // uniform-root contract on every live propagation.
        presentationConfig.requireUniformRootScale = false;
        presentationConfig.maxModules =
            (std::numeric_limits<uint32_t>::max)();
        presentationConfig.maxMovingParts =
            (std::numeric_limits<uint32_t>::max)();
        presentationConfig.minimumRootScale =
            (std::numeric_limits<float>::denorm_min)();
        presentationConfig.maximumRootScale =
            (std::numeric_limits<float>::max)();
        presentationConfig.maximumLocalMagnitude =
            (std::numeric_limits<float>::max)();
        const AssemblyPresentationResult presentation =
            StageAssemblyPresentation(
                instance->m_plan->RootTransform(),
                instance->m_plan->Modules(),
                instance->m_plan->MovingParts(),
                movingLocalTransforms,
                moduleWorldTransforms,
                movingPartWorldTransforms,
                presentationConfig);
        if (!presentation.Succeeded())
        {
            result.status = AssemblyInstantiationStatus::InvalidResolvedResources;
            return result;
        }

        const auto createTracked = [&]() {
            const ecs::Entity entity = target.CreateEntity();
            if (!entity.IsNull())
                staged.push_back(entity);
            return entity;
        };

        const ecs::Entity root = createTracked();
        if (root.IsNull() || !target.IsAlive(root))
        {
            RollbackEntities(target, staged);
            result.status = AssemblyInstantiationStatus::EntityCapacityExceeded;
            result.stagedEntityCount = staged.size();
            return result;
        }
        target.AssignTransform(root, instance->m_plan->RootTransform());
        target.AssignName(
            root, MakeName(instance->m_plan->Resources()->assembly->assetId));
        instance->m_rootEntity = root;

        for (const PreparedAssemblyModule& module : instance->m_plan->Modules())
        {
            const ecs::Entity entity = createTracked();
            if (entity.IsNull() || !target.IsAlive(entity))
            {
                RollbackEntities(target, staged);
                result.status =
                    AssemblyInstantiationStatus::EntityCapacityExceeded;
                result.stagedEntityCount = staged.size();
                return result;
            }
            target.AssignTransform(
                entity, moduleWorldTransforms[module.stableIndex]);
            target.AssignMeshInstance(
                entity,
                ecs::MeshInstance{ module.visual.runtime.mesh.value, true });
            target.AssignMaterial(entity, module.visual.runtime.material);
            target.AssignName(
                entity,
                MakeName(instance->m_plan->Resources()->assembly
                             ->modules[module.stableIndex].id));
            target.AssignParent(entity, ecs::Parent{ root.Index() });
            instance->m_moduleEntities.push_back(entity);
        }

        for (const PreparedAssemblyMovingPart& part :
             instance->m_plan->MovingParts())
        {
            if (part.moduleIndex >= instance->m_moduleEntities.size())
            {
                RollbackEntities(target, staged);
                result.status =
                    AssemblyInstantiationStatus::InvalidResolvedResources;
                result.stagedEntityCount = staged.size();
                return result;
            }
            const ecs::Entity entity = createTracked();
            if (entity.IsNull() || !target.IsAlive(entity))
            {
                RollbackEntities(target, staged);
                result.status =
                    AssemblyInstantiationStatus::EntityCapacityExceeded;
                result.stagedEntityCount = staged.size();
                return result;
            }
            target.AssignTransform(
                entity, movingPartWorldTransforms[part.stableIndex]);
            target.AssignMeshInstance(
                entity,
                ecs::MeshInstance{ part.visual.runtime.mesh.value, true });
            target.AssignMaterial(entity, part.visual.runtime.material);
            target.AssignName(
                entity,
                MakeName(instance->m_plan->Resources()->assembly
                             ->movingParts[part.stableIndex].id));
            target.AssignParent(
                entity,
                ecs::Parent{ instance->m_moduleEntities[part.moduleIndex].Index() });
            instance->m_movingPartEntities.push_back(entity);
        }

        instance->m_alive = true;
        result.status = AssemblyInstantiationStatus::Success;
        result.stagedEntityCount = staged.size();
        result.instance = std::move(instance);
        return result;
    }
    catch (const std::bad_alloc&)
    {
        RollbackEntities(target, staged);
        result.status = AssemblyInstantiationStatus::AllocationFailure;
        result.stagedEntityCount = staged.size();
        return result;
    }
    catch (...)
    {
        RollbackEntities(target, staged);
        result.status = AssemblyInstantiationStatus::InternalError;
        result.stagedEntityCount = staged.size();
        return result;
    }
}

AssemblyInstantiationStatus DestroyAssemblyInstance(
    AssemblyEntityTarget& target,
    AssemblyInstance& instance)
{
    if (!instance.m_alive)
        return AssemblyInstantiationStatus::Success;
    if (target.TargetIdentity() == nullptr ||
        target.TargetIdentity() != instance.m_targetIdentity)
    {
        return AssemblyInstantiationStatus::InvalidArgument;
    }

    for (auto it = instance.m_movingPartEntities.rbegin();
         it != instance.m_movingPartEntities.rend();
         ++it)
    {
        target.DestroyEntity(*it);
    }
    for (auto it = instance.m_moduleEntities.rbegin();
         it != instance.m_moduleEntities.rend();
         ++it)
    {
        target.DestroyEntity(*it);
    }
    target.DestroyEntity(instance.m_rootEntity);

    instance.m_movingPartEntities.clear();
    instance.m_moduleEntities.clear();
    instance.m_rootEntity = ecs::NullEntity;
    instance.m_plan.reset();
    instance.m_targetIdentity = nullptr;
    instance.m_alive = false;
    return AssemblyInstantiationStatus::Success;
}

} // namespace scene
