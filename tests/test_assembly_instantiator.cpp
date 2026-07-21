#include "test_framework.h"
#include "scene/assembly_instantiator.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{

asset::Sha256Digest HashText(std::string_view text)
{
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    return asset::ComputeSha256(std::span<const std::byte>(bytes, text.size()));
}

std::shared_ptr<asset::CookedAssembly> MakeAssembly()
{
    auto assembly = std::make_shared<asset::CookedAssembly>();
    assembly->schemaVersion = 1;
    assembly->assetKind = asset::AssemblyAssetKind::Ship;
    assembly->assetId = "ship.test.instantiation";
    assembly->assemblyRevision = "1";
    assembly->sourceManifestSha256 = HashText("assembly source");
    assembly->provenance.push_back(asset::AssemblyProvenance{
        "source", "test", HashText("request")
    });

    asset::AssemblyModule hull;
    hull.id = "hull";
    hull.provenanceIndex = 0;
    hull.visualSource = "visual://hull";
    hull.collisionSource = "collision://hull";
    hull.transform.positionMeters = { 1.0, 2.0, 3.0 };
    hull.transform.rotationEulerDegrees = { 0.0, 90.0, 0.0 };
    hull.transform.scale = { 1.0, 2.0, 1.0 };
    hull.lods.push_back(asset::AssemblyLod{ 1, "visual://hull-lod", 500.0 });
    assembly->modules.push_back(hull);

    asset::AssemblyModule interior;
    interior.id = "interior";
    interior.role = asset::AssemblyModuleRole::Interior;
    interior.provenanceIndex = 0;
    interior.visualSource = "visual://interior";
    interior.collisionSource = "collision://interior";
    interior.transform.positionMeters = { -2.0, 0.0, 1.0 };
    assembly->modules.push_back(interior);

    asset::AssemblyZone zone;
    zone.id = "cabin";
    zone.moduleIndex = 1;
    zone.navmeshSource = "nav://cabin";
    zone.walkableSurface = "walk://cabin";
    assembly->zones.push_back(zone);

    asset::AssemblyInteraction interaction;
    interaction.id = "hatch-control";
    interaction.moduleIndex = 1;
    interaction.states = { "closed", "open" };
    interaction.initialStateIndex = 0;
    interaction.movingPartIndex = 0;
    assembly->interactions.push_back(interaction);

    asset::AssemblyMovingPart part;
    part.id = "hatch";
    part.moduleIndex = 1;
    part.interactionIndex = 0;
    part.visualSource = "visual://hatch";
    assembly->movingParts.push_back(part);
    return assembly;
}

asset::AssemblyResourceRegistration Registration(
    asset::AssemblyResourceKind kind,
    std::string locator,
    std::string_view content,
    uint64_t ownerToken,
    std::shared_ptr<const void> anchor = std::make_shared<int>(1))
{
    asset::AssemblyResourceRegistration registration;
    registration.kind = kind;
    registration.locator = std::move(locator);
    registration.contentSha256 = HashText(content);
    registration.ownerToken = ownerToken;
    registration.lifetimeAnchor = std::move(anchor);
    return registration;
}

struct Fixture
{
    Fixture()
        : assembly(MakeAssembly()),
          catalog(std::make_unique<asset::AssemblyResourceCatalogStore>())
    {
        auto anchor = std::make_shared<int>(11);
        hullAnchor = anchor;
        Register(
            asset::AssemblyResourceKind::Visual,
            "visual://hull",
            101,
            anchor);
        Register(asset::AssemblyResourceKind::Visual, "visual://hull-lod", 102);
        Register(asset::AssemblyResourceKind::Visual, "visual://interior", 103);
        Register(asset::AssemblyResourceKind::Visual, "visual://hatch", 104);
        Register(asset::AssemblyResourceKind::Collision, "collision://hull", 201);
        Register(
            asset::AssemblyResourceKind::Collision,
            "collision://interior",
            202);
        Register(asset::AssemblyResourceKind::NavigationMesh, "nav://cabin", 301);
        Register(
            asset::AssemblyResourceKind::WalkableSurface,
            "walk://cabin",
            401);
        const auto acquired = catalog->AcquireSnapshot();
        CHECK(acquired.Succeeded());
        lease = acquired.snapshot;
    }

    void Register(
        asset::AssemblyResourceKind kind,
        std::string locator,
        uint64_t ownerToken,
        std::shared_ptr<const void> anchor = std::make_shared<int>(1))
    {
        const auto result = catalog->Register(Registration(
            kind, locator, locator, ownerToken, std::move(anchor)));
        CHECK(result.Succeeded());
    }

    std::shared_ptr<asset::CookedAssembly> assembly;
    std::unique_ptr<asset::AssemblyResourceCatalogStore> catalog;
    std::shared_ptr<const asset::AssemblyResourceCatalogSnapshot> lease;
    std::weak_ptr<const void> hullAnchor;
};

class FakeResourceAdapter final : public scene::AssemblyRuntimeResourceAdapter
{
public:
    scene::AssemblyRuntimeAdapterStatus PrepareVisual(
        const asset::AssemblyVisualIdentity&,
        uint64_t ownerToken,
        scene::PreparedAssemblyVisual& visual) const override
    {
        MaybeThrow();
        ++visualCalls;
        if (rejectKind == asset::AssemblyResourceKind::Visual)
            return rejectStatus;
        visual.mesh = invalidVisual ? scene::MeshHandle{} :
                                      scene::MeshHandle(
                                          static_cast<uint32_t>(ownerToken));
        visual.material.albedo = core::Color{ 0.2f, 0.3f, 0.4f, 1.0f };
        visual.material.roughness = invalidMaterial ?
            std::numeric_limits<float>::quiet_NaN() : 0.6f;
        visual.material.metallic = 0.2f;
        return unknownStatus ?
            static_cast<scene::AssemblyRuntimeAdapterStatus>(127) :
            scene::AssemblyRuntimeAdapterStatus::Success;
    }

    scene::AssemblyRuntimeAdapterStatus ValidateCollision(
        const asset::AssemblyCollisionIdentity&,
        uint64_t ownerToken) const override
    {
        MaybeThrow();
        ++collisionCalls;
        observedTokens.push_back(ownerToken);
        return StatusFor(asset::AssemblyResourceKind::Collision);
    }

    scene::AssemblyRuntimeAdapterStatus ValidateNavigationMesh(
        const asset::AssemblyNavigationMeshIdentity&,
        uint64_t ownerToken) const override
    {
        MaybeThrow();
        ++navigationCalls;
        observedTokens.push_back(ownerToken);
        return StatusFor(asset::AssemblyResourceKind::NavigationMesh);
    }

    scene::AssemblyRuntimeAdapterStatus ValidateWalkableSurface(
        const asset::AssemblyWalkableSurfaceIdentity&,
        uint64_t ownerToken) const override
    {
        MaybeThrow();
        ++walkableCalls;
        observedTokens.push_back(ownerToken);
        return StatusFor(asset::AssemblyResourceKind::WalkableSurface);
    }

    scene::AssemblyRuntimeAdapterStatus StatusFor(
        asset::AssemblyResourceKind kind) const
    {
        if (rejectKind == kind)
            return rejectStatus;
        return unknownStatus ?
            static_cast<scene::AssemblyRuntimeAdapterStatus>(127) :
            scene::AssemblyRuntimeAdapterStatus::Success;
    }

    void MaybeThrow() const
    {
        if (throwAllocation)
            throw std::bad_alloc();
        if (throwRuntime)
            throw std::runtime_error("adapter failure");
    }

    asset::AssemblyResourceKind rejectKind =
        static_cast<asset::AssemblyResourceKind>(0);
    scene::AssemblyRuntimeAdapterStatus rejectStatus =
        scene::AssemblyRuntimeAdapterStatus::InvalidResource;
    bool invalidVisual = false;
    bool invalidMaterial = false;
    bool unknownStatus = false;
    bool throwAllocation = false;
    bool throwRuntime = false;
    mutable uint32_t visualCalls = 0;
    mutable uint32_t collisionCalls = 0;
    mutable uint32_t navigationCalls = 0;
    mutable uint32_t walkableCalls = 0;
    mutable std::vector<uint64_t> observedTokens;
};

class FaultTarget final : public scene::AssemblyEntityTarget
{
public:
    ecs::Entity CreateEntity() override
    {
        if (created >= createLimit)
            return ecs::NullEntity;
        ++created;
        return registry.Create();
    }

    void DestroyEntity(ecs::Entity entity) noexcept override
    {
        if (registry.IsAlive(entity))
            ++destroyed;
        registry.Destroy(entity);
    }

    bool IsAlive(ecs::Entity entity) const noexcept override
    {
        return registry.IsAlive(entity);
    }

    const void* TargetIdentity() const noexcept override
    {
        return &registry;
    }

    void AssignTransform(
        ecs::Entity entity,
        const ecs::Transform& value) override
    {
        BeforeAssign();
        registry.Assign<ecs::Transform>(entity, value);
    }

    void AssignMeshInstance(
        ecs::Entity entity,
        const ecs::MeshInstance& value) override
    {
        BeforeAssign();
        registry.Assign<ecs::MeshInstance>(entity, value);
    }

    void AssignMaterial(
        ecs::Entity entity,
        const ecs::Material& value) override
    {
        BeforeAssign();
        registry.Assign<ecs::Material>(entity, value);
    }

    void AssignName(ecs::Entity entity, const ecs::Name& value) override
    {
        BeforeAssign();
        registry.Assign<ecs::Name>(entity, value);
    }

    void AssignParent(ecs::Entity entity, const ecs::Parent& value) override
    {
        BeforeAssign();
        registry.Assign<ecs::Parent>(entity, value);
    }

    void BeforeAssign()
    {
        if (assignCalls++ == throwOnAssign)
            throw std::runtime_error("target assignment failure");
    }

    ecs::Registry registry;
    uint64_t createLimit = (std::numeric_limits<uint64_t>::max)();
    uint64_t throwOnAssign = (std::numeric_limits<uint64_t>::max)();
    uint64_t created = 0;
    uint64_t destroyed = 0;
    uint64_t assignCalls = 0;
};

scene::AssemblyPreparationResult Prepare(
    Fixture& fixture,
    const FakeResourceAdapter& adapter,
    const ecs::Transform& root = {},
    const scene::AssemblyInstantiationLimits& limits = {})
{
    return scene::PrepareAssemblyInstance(
        fixture.assembly, fixture.lease, adapter, root, limits);
}

} // namespace

TEST_CASE(AssemblyInstantiator_PreparesEveryBindingAndPreservesLocalTransforms)
{
    Fixture fixture;
    FakeResourceAdapter adapter;
    ecs::Transform root;
    root.position = { 10.0, 20.0, 30.0 };
    root.rotation = core::Quatf::FromEuler(0.0f, 1.57079632679f, 0.0f);
    root.scale = { 2.0f, 3.0f, 4.0f };

    const auto result = Prepare(fixture, adapter, root);

    CHECK(result.Succeeded());
    CHECK(result.plan->Resources()->assembly == fixture.assembly);
    CHECK(result.plan->CatalogLease() == fixture.lease);
    CHECK_EQ(result.plan->EntityCount(), (uint64_t)4);
    CHECK_EQ(result.plan->BindingCount(), (uint64_t)8);
    CHECK_EQ(result.plan->Modules().size(), (size_t)2);
    CHECK_EQ(result.plan->Zones().size(), (size_t)1);
    CHECK_EQ(result.plan->MovingParts().size(), (size_t)1);
    CHECK_EQ(adapter.visualCalls, (uint32_t)4);
    CHECK_EQ(adapter.collisionCalls, (uint32_t)2);
    CHECK_EQ(adapter.navigationCalls, (uint32_t)1);
    CHECK_EQ(adapter.walkableCalls, (uint32_t)1);

    const ecs::Transform& hull = result.plan->Modules()[0].localTransform;
    CHECK_APPROX_EPS(hull.position.x, 1.0, 1.0e-8);
    CHECK_APPROX_EPS(hull.position.y, 2.0, 1.0e-8);
    CHECK_APPROX_EPS(hull.position.z, 3.0, 1.0e-8);
    CHECK_APPROX_EPS(hull.scale.x, 1.0f, 1.0e-6f);
    CHECK_APPROX_EPS(hull.scale.y, 2.0f, 1.0e-6f);
    CHECK_APPROX_EPS(hull.scale.z, 1.0f, 1.0e-6f);
    CHECK_EQ(result.plan->Modules()[0].visual.catalog.ownerToken, (uint64_t)101);
    CHECK_EQ(result.plan->Modules()[0].collision.ownerToken, (uint64_t)201);
    CHECK_EQ(result.plan->Modules()[0].lods[0].catalog.ownerToken, (uint64_t)102);
    CHECK_EQ(result.plan->Zones()[0].navigationMesh.ownerToken, (uint64_t)301);
    CHECK_EQ(result.plan->Zones()[0].walkableSurface.ownerToken, (uint64_t)401);
    CHECK_EQ(result.plan->MovingParts()[0].visual.catalog.ownerToken, (uint64_t)104);
    CHECK_EQ(
        result.plan->MovingParts()[0].localTransform.position.x,
        result.plan->Modules()[1].localTransform.position.x);
}

TEST_CASE(AssemblyInstantiator_CommitsStableEntityMappingsAndDestroysIdempotently)
{
    Fixture fixture;
    FakeResourceAdapter adapter;
    ecs::Transform genericRoot;
    genericRoot.position = { 100.0, 200.0, 300.0 };
    genericRoot.scale = { 2.0e6f, 3.0e6f, 4.0e6f };
    auto prepared = Prepare(fixture, adapter, genericRoot);
    CHECK(prepared.Succeeded());

    ecs::Registry registry;
    scene::RegistryAssemblyEntityTarget target(registry);
    auto committed = scene::CommitPreparedAssembly(target, prepared.plan);

    CHECK(committed.Succeeded());
    CHECK_EQ(committed.stagedEntityCount, (uint64_t)4);
    CHECK_EQ(registry.EntityCount(), (uint32_t)4);
    CHECK(registry.Has<ecs::Transform>(committed.instance->RootEntity()));
    CHECK(registry.Has<ecs::Name>(committed.instance->RootEntity()));
    CHECK_FALSE(registry.Has<ecs::MeshInstance>(committed.instance->RootEntity()));
    CHECK_EQ(committed.instance->ModuleEntities().size(), (size_t)2);
    CHECK_EQ(committed.instance->MovingPartEntities().size(), (size_t)1);

    const ecs::Transform& committedHull = registry.Get<ecs::Transform>(
        committed.instance->ModuleEntities()[0]);
    CHECK_APPROX_EPS(committedHull.position.x, 2'000'100.0, 1.0e-6);
    CHECK_APPROX_EPS(committedHull.position.y, 6'000'200.0, 1.0e-6);
    CHECK_APPROX_EPS(committedHull.position.z, 12'000'300.0, 1.0e-6);

    for (size_t i = 0; i < committed.instance->ModuleEntities().size(); ++i)
    {
        const ecs::Entity entity = committed.instance->ModuleEntities()[i];
        CHECK(registry.Has<ecs::Transform>(entity));
        CHECK(registry.Has<ecs::MeshInstance>(entity));
        CHECK(registry.Has<ecs::Material>(entity));
        CHECK_EQ(
            registry.Get<ecs::Parent>(entity).entityIndex,
            committed.instance->RootEntity().Index());
        CHECK_EQ(
            registry.Get<ecs::MeshInstance>(entity).meshHandle,
            prepared.plan->Modules()[i].visual.runtime.mesh.value);
    }

    const ecs::Entity part = committed.instance->MovingPartEntities()[0];
    CHECK_EQ(
        registry.Get<ecs::Parent>(part).entityIndex,
        committed.instance->ModuleEntities()[1].Index());
    CHECK(committed.instance->Plan() == prepared.plan);

    prepared.plan.reset();
    fixture.lease.reset();
    fixture.catalog.reset();
    CHECK_FALSE(fixture.hullAnchor.expired());

    ecs::Registry wrongRegistry;
    scene::RegistryAssemblyEntityTarget wrongTarget(wrongRegistry);
    CHECK_EQ(
        scene::DestroyAssemblyInstance(wrongTarget, *committed.instance),
        scene::AssemblyInstantiationStatus::InvalidArgument);
    CHECK_EQ(registry.EntityCount(), (uint32_t)4);

    scene::RegistryAssemblyEntityTarget teardownTarget(registry);
    CHECK_EQ(
        scene::DestroyAssemblyInstance(teardownTarget, *committed.instance),
        scene::AssemblyInstantiationStatus::Success);
    CHECK_FALSE(committed.instance->IsAlive());
    CHECK(committed.instance->Plan() == nullptr);
    CHECK_EQ(registry.EntityCount(), (uint32_t)0);
    CHECK(fixture.hullAnchor.expired());
    CHECK_EQ(
        scene::DestroyAssemblyInstance(target, *committed.instance),
        scene::AssemblyInstantiationStatus::Success);
    CHECK_EQ(registry.EntityCount(), (uint32_t)0);
}

TEST_CASE(AssemblyInstantiator_RollsBackPartialTargetFailureAndCapacityFailure)
{
    Fixture fixture;
    FakeResourceAdapter adapter;
    const auto prepared = Prepare(fixture, adapter);
    CHECK(prepared.Succeeded());

    FaultTarget throwing;
    throwing.throwOnAssign = 4;
    const auto failed = scene::CommitPreparedAssembly(throwing, prepared.plan);
    CHECK_FALSE(failed.Succeeded());
    CHECK_EQ(failed.status, scene::AssemblyInstantiationStatus::InternalError);
    CHECK(failed.stagedEntityCount >= (uint64_t)2);
    CHECK_EQ(throwing.registry.EntityCount(), (uint32_t)0);
    CHECK_EQ(throwing.destroyed, failed.stagedEntityCount);

    FaultTarget exhausted;
    exhausted.createLimit = 2;
    const auto capacity = scene::CommitPreparedAssembly(exhausted, prepared.plan);
    CHECK_FALSE(capacity.Succeeded());
    CHECK_EQ(
        capacity.status,
        scene::AssemblyInstantiationStatus::EntityCapacityExceeded);
    CHECK_EQ(capacity.stagedEntityCount, (uint64_t)2);
    CHECK_EQ(exhausted.registry.EntityCount(), (uint32_t)0);
    CHECK_EQ(exhausted.destroyed, (uint64_t)2);
}

TEST_CASE(AssemblyInstantiator_RejectsAdapterFailuresBeforeSceneMutation)
{
    Fixture fixture;
    FakeResourceAdapter adapter;
    adapter.rejectKind = asset::AssemblyResourceKind::Collision;
    adapter.rejectStatus = scene::AssemblyRuntimeAdapterStatus::Stale;
    const auto rejected = Prepare(fixture, adapter);
    CHECK_FALSE(rejected.Succeeded());
    CHECK_EQ(
        rejected.status,
        scene::AssemblyInstantiationStatus::AdapterRejected);
    CHECK_EQ(rejected.failedKind, asset::AssemblyResourceKind::Collision);
    CHECK_EQ(rejected.failedStableIndex, (uint32_t)0);
    CHECK_EQ(rejected.adapterStatus, scene::AssemblyRuntimeAdapterStatus::Stale);

    adapter.rejectKind = static_cast<asset::AssemblyResourceKind>(0);
    adapter.throwAllocation = true;
    const auto allocation = Prepare(fixture, adapter);
    CHECK_EQ(
        allocation.status,
        scene::AssemblyInstantiationStatus::AllocationFailure);
    adapter.throwAllocation = false;
    adapter.throwRuntime = true;
    const auto exception = Prepare(fixture, adapter);
    CHECK_EQ(exception.status, scene::AssemblyInstantiationStatus::InternalError);

    FakeResourceAdapter invalidVisual;
    invalidVisual.invalidVisual = true;
    CHECK_EQ(
        Prepare(fixture, invalidVisual).status,
        scene::AssemblyInstantiationStatus::InvalidRuntimeBinding);
    invalidVisual.invalidVisual = false;
    invalidVisual.invalidMaterial = true;
    CHECK_EQ(
        Prepare(fixture, invalidVisual).status,
        scene::AssemblyInstantiationStatus::InvalidRuntimeBinding);
    invalidVisual.invalidMaterial = false;
    invalidVisual.unknownStatus = true;
    CHECK_EQ(
        Prepare(fixture, invalidVisual).status,
        scene::AssemblyInstantiationStatus::InternalError);
}

TEST_CASE(AssemblyInstantiator_RequiresTheLeaseDuringResolutionAndPlanLifetime)
{
    Fixture fixture;
    FakeResourceAdapter adapter;
    auto missingAssembly = fixture.assembly;
    missingAssembly->modules[0].visualSource = "visual://not-in-lease";
    const auto missing = Prepare(fixture, adapter);
    CHECK_FALSE(missing.Succeeded());
    CHECK_EQ(
        missing.status,
        scene::AssemblyInstantiationStatus::CatalogLeaseMismatch);
    CHECK_EQ(
        missing.resolutionStatus,
        asset::AssemblyResourceResolutionStatus::CatalogNotFound);

    fixture.assembly = MakeAssembly();
    auto prepared = Prepare(fixture, adapter);
    CHECK(prepared.Succeeded());
    CHECK_FALSE(fixture.hullAnchor.expired());
    fixture.lease.reset();
    fixture.catalog.reset();
    CHECK_FALSE(fixture.hullAnchor.expired());
    CHECK(prepared.plan->Resources()->assembly == fixture.assembly);
    prepared.plan.reset();
    CHECK(fixture.hullAnchor.expired());
}

TEST_CASE(AssemblyInstantiator_EnforcesLimitsAndInvalidTransforms)
{
    Fixture fixture;
    FakeResourceAdapter adapter;

    scene::AssemblyInstantiationLimits limits;
    limits.maxEntities = 3;
    CHECK_EQ(
        Prepare(fixture, adapter, {}, limits).status,
        scene::AssemblyInstantiationStatus::ResourceLimitExceeded);
    limits = {};
    limits.maxBindings = 7;
    CHECK_EQ(
        Prepare(fixture, adapter, {}, limits).status,
        scene::AssemblyInstantiationStatus::ResourceLimitExceeded);
    limits = {};
    limits.resolution.maxBindings = 1;
    const auto resolutionLimit = Prepare(fixture, adapter, {}, limits);
    CHECK_EQ(
        resolutionLimit.status,
        scene::AssemblyInstantiationStatus::ResourceLimitExceeded);
    CHECK_EQ(
        resolutionLimit.resolutionStatus,
        asset::AssemblyResourceResolutionStatus::ResourceLimitExceeded);
    CHECK_EQ(adapter.visualCalls, (uint32_t)0);
    limits = {};
    limits.maxLodsPerModule = 0;
    CHECK_EQ(
        Prepare(fixture, adapter, {}, limits).status,
        scene::AssemblyInstantiationStatus::InvalidArgument);

    ecs::Transform invalidRoot;
    invalidRoot.rotation = { 0.0f, 0.0f, 0.0f, 0.0f };
    CHECK_EQ(
        Prepare(fixture, adapter, invalidRoot).status,
        scene::AssemblyInstantiationStatus::InvalidArgument);

    CHECK_EQ(
        scene::PrepareAssemblyInstance(
            nullptr, fixture.lease, adapter).status,
        scene::AssemblyInstantiationStatus::InvalidArgument);
    CHECK_EQ(
        scene::PrepareAssemblyInstance(
            fixture.assembly, nullptr, adapter).status,
        scene::AssemblyInstantiationStatus::InvalidArgument);
    invalidRoot = {};
    invalidRoot.scale.x = -1.0f;
    CHECK_EQ(
        Prepare(fixture, adapter, invalidRoot).status,
        scene::AssemblyInstantiationStatus::InvalidArgument);

    fixture.assembly->modules[0].transform.scale[0] =
        std::numeric_limits<double>::infinity();
    CHECK_EQ(
        Prepare(fixture, adapter).status,
        scene::AssemblyInstantiationStatus::InvalidResolvedResources);
}

TEST_CASE(AssemblyInstantiator_StatusNamesRemainStable)
{
    CHECK_EQ(
        std::string(scene::AssemblyInstantiationStatusName(
            scene::AssemblyInstantiationStatus::CatalogLeaseMismatch)),
        "catalog lease mismatch");
    CHECK_EQ(
        std::string(scene::AssemblyRuntimeAdapterStatusName(
            scene::AssemblyRuntimeAdapterStatus::InvalidResource)),
        "invalid resource");
    CHECK_EQ(
        std::string(scene::AssemblyInstantiationStatusName(
            static_cast<scene::AssemblyInstantiationStatus>(127))),
        "unknown");
    CHECK_EQ(
        std::string(scene::AssemblyRuntimeAdapterStatusName(
            static_cast<scene::AssemblyRuntimeAdapterStatus>(127))),
        "unknown");
}
