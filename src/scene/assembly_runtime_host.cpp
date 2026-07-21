#include "assembly_runtime_host.h"

#include "../core/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <string_view>
#include <utility>

namespace scene
{

namespace
{

constexpr double kDegreesToRadians = 0.017453292519943295769;

AssemblyRuntimeHostResult Failure(
    AssemblyRuntimeHostStatus status,
    std::string error)
{
    AssemblyRuntimeHostResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

AssemblyInteriorResult InteriorFailure(
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

bool IsNonZero(const asset::Sha256Digest& digest)
{
    for (uint8_t byte : digest.bytes)
        if (byte != 0)
            return true;
    return false;
}

asset::Sha256Digest ContractDigest(
    asset::AssemblyResourceKind kind,
    std::string_view locator)
{
    std::vector<std::byte> bytes;
    bytes.reserve(locator.size() + 1u);
    bytes.push_back(static_cast<std::byte>(kind));
    for (char value : locator)
        bytes.push_back(static_cast<std::byte>(static_cast<uint8_t>(value)));
    return asset::ComputeSha256(bytes);
}

asset::Sha256Digest VisualDigest(
    const asset::Sha256Digest& modelSource,
    uint32_t primitiveIndex)
{
    std::array<std::byte, 36> bytes{};
    for (size_t i = 0; i < modelSource.bytes.size(); ++i)
        bytes[i] = static_cast<std::byte>(modelSource.bytes[i]);
    bytes[32] = static_cast<std::byte>(primitiveIndex);
    bytes[33] = static_cast<std::byte>(primitiveIndex >> 8);
    bytes[34] = static_cast<std::byte>(primitiveIndex >> 16);
    bytes[35] = static_cast<std::byte>(primitiveIndex >> 24);
    return asset::ComputeSha256(bytes);
}

bool ToRootTransform(
    const asset::AssemblyTransform& source,
    ecs::Transform& transform)
{
    const double maxFloat = (std::numeric_limits<float>::max)();
    for (double value : source.positionMeters)
        if (!std::isfinite(value))
            return false;
    for (double value : source.rotationEulerDegrees)
        if (!std::isfinite(value))
            return false;
    for (double value : source.scale)
        if (!std::isfinite(value) || value <= 0.0 || value > maxFloat)
            return false;

    transform.position = {
        source.positionMeters[0],
        source.positionMeters[1],
        source.positionMeters[2]
    };
    const float pitch = static_cast<float>(
        std::remainder(source.rotationEulerDegrees[0], 360.0) *
        kDegreesToRadians);
    const float yaw = static_cast<float>(
        std::remainder(source.rotationEulerDegrees[1], 360.0) *
        kDegreesToRadians);
    const float roll = static_cast<float>(
        std::remainder(source.rotationEulerDegrees[2], 360.0) *
        kDegreesToRadians);
    transform.rotation = core::Quatf::FromEuler(pitch, yaw, roll).Normalized();
    transform.scale = {
        static_cast<float>(source.scale[0]),
        static_cast<float>(source.scale[1]),
        static_cast<float>(source.scale[2])
    };
    return true;
}

const LoadedModelPrimitive* FindPrimitive(
    const LoadedModelResources& model,
    uint32_t sourcePrimitiveIndex)
{
    const auto it = std::find_if(
        model.primitives.begin(), model.primitives.end(),
        [sourcePrimitiveIndex](const LoadedModelPrimitive& primitive) {
            return primitive.sourcePrimitiveIndex == sourcePrimitiveIndex;
        });
    return it == model.primitives.end() ? nullptr : &*it;
}

bool ResolveConfinedContentPath(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    std::filesystem::path& resolved)
{
    std::error_code error;
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(root, error);
    if (error || canonicalRoot.empty())
        return false;
    resolved = std::filesystem::weakly_canonical(root / relative, error);
    if (error || resolved.empty())
        return false;
    const std::filesystem::path fromRoot =
        resolved.lexically_relative(canonicalRoot);
    if (fromRoot.empty() || fromRoot.is_absolute())
        return false;
    const auto first = fromRoot.begin();
    return first != fromRoot.end() && *first != "..";
}

} // namespace

const char* AssemblyRuntimeHostStatusName(AssemblyRuntimeHostStatus status)
{
    switch (status)
    {
    case AssemblyRuntimeHostStatus::Success: return "success";
    case AssemblyRuntimeHostStatus::InvalidState: return "invalid state";
    case AssemblyRuntimeHostStatus::ManifestFailure: return "manifest failure";
    case AssemblyRuntimeHostStatus::AssemblyFailure: return "assembly failure";
    case AssemblyRuntimeHostStatus::CoverageFailure: return "coverage failure";
    case AssemblyRuntimeHostStatus::ModelFailure: return "model failure";
    case AssemblyRuntimeHostStatus::ResourceFailure: return "resource failure";
    case AssemblyRuntimeHostStatus::CatalogFailure: return "catalog failure";
    case AssemblyRuntimeHostStatus::PreparationFailure: return "preparation failure";
    case AssemblyRuntimeHostStatus::CommitFailure: return "commit failure";
    case AssemblyRuntimeHostStatus::InteriorFailure: return "interior failure";
    case AssemblyRuntimeHostStatus::AllocationFailure: return "allocation failure";
    case AssemblyRuntimeHostStatus::InternalError: return "internal error";
    default: return "unknown";
    }
}

AssemblyRuntimeHostResult AssemblyRuntimeHost::BeginLoad(
    Scene& scene,
    render::D3D12Device& device,
    render::Renderer& renderer,
    const std::filesystem::path& manifestPath)
{
    if (m_pending || m_instance || m_plan || m_catalog || m_owners ||
        m_interior.IsInitialized() || !m_models.empty())
    {
        return Failure(
            AssemblyRuntimeHostStatus::InvalidState,
            "runtime assembly host is already in use");
    }

    try
    {
        const asset::RuntimeContentManifestResult manifestResult =
            asset::LoadRuntimeContentManifestFile(manifestPath);
        if (!manifestResult.Succeeded())
        {
            return Failure(
                AssemblyRuntimeHostStatus::ManifestFailure,
                manifestResult.error.empty()
                    ? asset::RuntimeContentManifestStatusName(manifestResult.status)
                    : manifestResult.error);
        }
        m_manifest = manifestResult.manifest;

        std::filesystem::path assemblyPath;
        if (!ResolveConfinedContentPath(
                m_manifest.contentRoot,
                m_manifest.cookedAssemblyPath,
                assemblyPath))
        {
            ReleaseState(scene, device, renderer);
            return Failure(
                AssemblyRuntimeHostStatus::ManifestFailure,
                "cooked assembly path escapes the content root");
        }
        const asset::CookedAssemblyResult assemblyResult =
            asset::LoadCookedAssemblyFile(assemblyPath);
        if (!assemblyResult.Succeeded())
        {
            ReleaseState(scene, device, renderer);
            return Failure(
                AssemblyRuntimeHostStatus::AssemblyFailure,
                assemblyResult.error.empty()
                    ? asset::CookedAssemblyStatusName(assemblyResult.status)
                    : assemblyResult.error);
        }
        m_assembly = assemblyResult.assembly;

        const asset::RuntimeContentCoverageResult coverage =
            asset::ValidateRuntimeContentCoverage(m_manifest, *m_assembly);
        if (!coverage.matched)
        {
            const std::string error = coverage.error +
                (coverage.failedLocator.empty()
                    ? std::string()
                    : ": " + coverage.failedLocator);
            ReleaseState(scene, device, renderer);
            return Failure(AssemblyRuntimeHostStatus::CoverageFailure, error);
        }

        std::map<std::filesystem::path, size_t> modelIndices;
        m_models.reserve(m_manifest.bindings.size());
        for (const asset::RuntimeContentBinding& binding : m_manifest.bindings)
        {
            if (!binding.IsVisual())
                continue;
            std::filesystem::path modelPath;
            if (!ResolveConfinedContentPath(
                    m_manifest.contentRoot, binding.cookedModelPath, modelPath))
            {
                ReleaseState(scene, device, renderer);
                return Failure(
                    AssemblyRuntimeHostStatus::ManifestFailure,
                    "cooked model path escapes the content root: " +
                        binding.locator);
            }
            auto [it, inserted] = modelIndices.emplace(modelPath, m_models.size());
            if (inserted)
            {
                LoadedModelResources loaded =
                    LoadCookedModelResources(scene, device, renderer, modelPath);
                if (!loaded.ok)
                {
                    const std::string error = loaded.error.empty()
                        ? "cooked visual resource failed to load"
                        : loaded.error;
                    ReleaseLoadedModelResources(loaded, scene, device, renderer);
                    ReleaseState(scene, device, renderer);
                    return Failure(AssemblyRuntimeHostStatus::ModelFailure, error);
                }
                core::Log::Infof(
                    "[SMOKE] model_loaded=ok model_source=cooked model_primitives=%zu model_vertices=%llu model_indices=%llu model_images=%u",
                    loaded.primitives.size(),
                    static_cast<unsigned long long>(loaded.vertexCount),
                    static_cast<unsigned long long>(loaded.indexCount),
                    loaded.decodedImageCount);
                m_models.push_back(std::move(loaded));
            }
        }

        m_owners = std::make_shared<AssemblyRuntimeResourceOwners>();
        m_catalog = std::make_unique<asset::AssemblyResourceCatalogStore>();
        const std::shared_ptr<const void> lifetimeAnchor = m_owners;

        for (const asset::RuntimeContentBinding& binding : m_manifest.bindings)
        {
            AssemblyRuntimeOwnerRegistration ownerRegistration;
            ownerRegistration.kind = binding.kind;
            ownerRegistration.locator = binding.locator;

            if (binding.IsVisual())
            {
                std::filesystem::path modelPath;
                if (!ResolveConfinedContentPath(
                        m_manifest.contentRoot, binding.cookedModelPath, modelPath))
                {
                    ReleaseState(scene, device, renderer);
                    return Failure(
                        AssemblyRuntimeHostStatus::ManifestFailure,
                        "cooked model path escapes the content root: " +
                            binding.locator);
                }
                const auto modelIt = modelIndices.find(modelPath);
                if (modelIt == modelIndices.end() || modelIt->second >= m_models.size())
                {
                    ReleaseState(scene, device, renderer);
                    return Failure(
                        AssemblyRuntimeHostStatus::InternalError,
                        "visual model index disappeared during registration");
                }
                const LoadedModelResources& model = m_models[modelIt->second];
                const LoadedModelPrimitive* primitive =
                    FindPrimitive(model, binding.primitiveIndex);
                if (!primitive || !IsNonZero(model.sourceSha256))
                {
                    ReleaseState(scene, device, renderer);
                    return Failure(
                        AssemblyRuntimeHostStatus::ResourceFailure,
                        "visual primitive selection or source identity is invalid: " +
                            binding.locator);
                }
                ownerRegistration.contentSha256 =
                    VisualDigest(model.sourceSha256, binding.primitiveIndex);
                ownerRegistration.visual.mesh = primitive->mesh;
                ownerRegistration.visual.material = primitive->material;
            }
            else
            {
                ownerRegistration.contentSha256 =
                    ContractDigest(binding.kind, binding.locator);
            }

            const AssemblyRuntimeOwnerMutationResult ownerResult =
                m_owners->Register(ownerRegistration);
            if (!ownerResult.Succeeded())
            {
                const std::string error = std::string("owner registration failed: ") +
                    AssemblyRuntimeOwnerStatusName(ownerResult.status) + ": " +
                    binding.locator;
                ReleaseState(scene, device, renderer);
                return Failure(AssemblyRuntimeHostStatus::ResourceFailure, error);
            }

            asset::AssemblyResourceRegistration catalogRegistration;
            catalogRegistration.kind = binding.kind;
            catalogRegistration.locator = binding.locator;
            catalogRegistration.contentSha256 = ownerRegistration.contentSha256;
            catalogRegistration.ownerToken = ownerResult.ownerToken;
            catalogRegistration.lifetimeAnchor = lifetimeAnchor;
            const asset::AssemblyResourceCatalogMutationResult catalogResult =
                m_catalog->Register(catalogRegistration);
            if (!catalogResult.Succeeded())
            {
                const std::string error = std::string("catalog registration failed: ") +
                    asset::AssemblyResourceCatalogStatusName(catalogResult.status) +
                    ": " + binding.locator;
                ReleaseState(scene, device, renderer);
                return Failure(AssemblyRuntimeHostStatus::CatalogFailure, error);
            }
            const AssemblyRuntimeOwnerStatus bindStatus =
                m_owners->BindCatalogIdentity(
                    ownerResult.ownerToken, catalogResult.identity);
            if (bindStatus != AssemblyRuntimeOwnerStatus::Success)
            {
                const std::string error = std::string("owner identity bind failed: ") +
                    AssemblyRuntimeOwnerStatusName(bindStatus) + ": " +
                    binding.locator;
                ReleaseState(scene, device, renderer);
                return Failure(AssemblyRuntimeHostStatus::CatalogFailure, error);
            }
        }

        const AssemblyRuntimeOwnerStatus sealStatus = m_owners->Seal();
        if (sealStatus != AssemblyRuntimeOwnerStatus::Success)
        {
            const std::string error = std::string("owner table seal failed: ") +
                AssemblyRuntimeOwnerStatusName(sealStatus);
            ReleaseState(scene, device, renderer);
            return Failure(AssemblyRuntimeHostStatus::ResourceFailure, error);
        }

        const asset::AssemblyResourceCatalogSnapshotResult snapshot =
            m_catalog->AcquireSnapshot();
        if (!snapshot.Succeeded())
        {
            const std::string error = snapshot.error.empty()
                ? asset::AssemblyResourceCatalogStatusName(snapshot.status)
                : snapshot.error;
            ReleaseState(scene, device, renderer);
            return Failure(AssemblyRuntimeHostStatus::CatalogFailure, error);
        }

        ecs::Transform rootTransform;
        if (!ToRootTransform(m_manifest.rootTransform, rootTransform))
        {
            ReleaseState(scene, device, renderer);
            return Failure(
                AssemblyRuntimeHostStatus::ManifestFailure,
                "root transform cannot be represented by the runtime");
        }
        const AssemblyPreparationResult prepared = PrepareAssemblyInstance(
            m_assembly, snapshot.snapshot, *m_owners, rootTransform);
        if (!prepared.Succeeded())
        {
            const std::string error = std::string("assembly preparation failed: ") +
                AssemblyInstantiationStatusName(prepared.status);
            ReleaseState(scene, device, renderer);
            return Failure(AssemblyRuntimeHostStatus::PreparationFailure, error);
        }
        m_plan = prepared.plan;
        m_pending = true;

        core::Log::Infof(
            "[SMOKE] runtime_content_prepared=ok scene=%s bindings=%zu models=%zu",
            m_manifest.sceneId.c_str(),
            m_manifest.bindings.size(),
            m_models.size());
        return { AssemblyRuntimeHostStatus::Success, {} };
    }
    catch (const std::bad_alloc&)
    {
        ReleaseState(scene, device, renderer);
        return Failure(
            AssemblyRuntimeHostStatus::AllocationFailure,
            "allocation failure while loading runtime assembly");
    }
    catch (...)
    {
        ReleaseState(scene, device, renderer);
        return Failure(
            AssemblyRuntimeHostStatus::InternalError,
            "unexpected runtime assembly load failure");
    }
}

AssemblyRuntimeHostResult AssemblyRuntimeHost::CommitAfterUploadRetirement(
    Scene& scene)
{
    if (!m_pending || !m_plan || m_instance)
    {
        return Failure(
            AssemblyRuntimeHostStatus::InvalidState,
            "runtime assembly host has no prepared upload batch");
    }

    for (LoadedModelResources& model : m_models)
        model.uploadBuffers.clear();

    RegistryAssemblyEntityTarget target(scene.GetRegistry());
    const AssemblyCommitResult committed =
        CommitPreparedAssembly(target, m_plan);
    if (!committed.Succeeded())
    {
        return Failure(
            AssemblyRuntimeHostStatus::CommitFailure,
            std::string("assembly commit failed: ") +
                AssemblyInstantiationStatusName(committed.status));
    }

    const AssemblyInteriorResult interiorInitialized = m_interior.Initialize(
        m_assembly, m_plan->Modules(), m_plan->MovingParts());
    if (!interiorInitialized.Succeeded())
    {
        DestroyAssemblyInstance(target, *committed.instance);
        m_interior.Shutdown();
        m_plan.reset();
        m_pending = false;
        return Failure(
            AssemblyRuntimeHostStatus::InteriorFailure,
            std::string("interior runtime initialization failed: ") +
                AssemblyInteriorStatusName(interiorInitialized.status) +
                (interiorInitialized.error.empty()
                    ? std::string{}
                    : std::string(" (") + interiorInitialized.error + ")"));
    }

    m_instance = committed.instance;
    const AssemblyInteriorResult initialTransforms =
        ApplyInteriorTransforms(scene);
    if (!initialTransforms.Succeeded())
    {
        DestroyAssemblyInstance(target, *m_instance);
        m_instance.reset();
        m_interior.Shutdown();
        m_plan.reset();
        m_pending = false;
        return Failure(
            AssemblyRuntimeHostStatus::InteriorFailure,
            std::string("interior ECS publication failed: ") +
                initialTransforms.error);
    }

    m_plan.reset();
    m_pending = false;
    core::Log::Infof(
        "[SMOKE] runtime_assembly_committed=ok asset=%s modules=%zu moving_parts=%zu entities=%llu",
        m_assembly->assetId.c_str(),
        m_assembly->modules.size(),
        m_assembly->movingParts.size(),
        static_cast<unsigned long long>(committed.stagedEntityCount));
    core::Log::Infof(
        "[SMOKE] interior_runtime_ready=ok interactions=%zu portals=%zu moving_parts=%zu",
        m_interior.InteractionCount(),
        m_interior.PortalCount(),
        m_interior.MovingPartCount());
    return { AssemblyRuntimeHostStatus::Success, {} };
}

AssemblyInteriorResult AssemblyRuntimeHost::ValidateInteriorEntities(
    Scene& scene) const
{
    if (!m_instance || !m_instance->IsAlive() || !m_interior.IsInitialized())
    {
        return InteriorFailure(
            AssemblyInteriorStatus::NotInitialized,
            "interior host is not live");
    }
    const std::span<const ecs::Entity> entities =
        m_instance->MovingPartEntities();
    if (entities.size() != m_interior.MovingPartCount())
    {
        return InteriorFailure(
            AssemblyInteriorStatus::InvalidTopology,
            "interior moving-part entity count does not match runtime topology");
    }

    ecs::Registry& registry = scene.GetRegistry();
    for (size_t i = 0; i < entities.size(); ++i)
    {
        if (!registry.IsAlive(entities[i]) ||
            registry.TryGet<ecs::Transform>(entities[i]) == nullptr)
        {
            return InteriorFailure(
                AssemblyInteriorStatus::InvalidTopology,
                "interior moving-part entity or transform is unavailable",
                static_cast<uint32_t>(i));
        }
        if (m_interior.MovingPartTransform(static_cast<uint32_t>(i)) == nullptr)
        {
            return InteriorFailure(
                AssemblyInteriorStatus::InvalidTopology,
                "interior moving-part transform is unavailable",
                static_cast<uint32_t>(i));
        }
    }
    return { AssemblyInteriorStatus::Success };
}

AssemblyInteriorResult AssemblyRuntimeHost::ApplyInteriorTransforms(
    Scene& scene) const
{
    const AssemblyInteriorResult validated = ValidateInteriorEntities(scene);
    if (!validated.Succeeded())
        return validated;

    ecs::Registry& registry = scene.GetRegistry();
    const std::span<const ecs::Entity> entities =
        m_instance->MovingPartEntities();
    for (size_t i = 0; i < entities.size(); ++i)
    {
        *registry.TryGet<ecs::Transform>(entities[i]) =
            *m_interior.MovingPartTransform(static_cast<uint32_t>(i));
    }
    return { AssemblyInteriorStatus::Success };
}

AssemblyInteriorResult AssemblyRuntimeHost::ActivateInteraction(
    Scene& scene,
    uint32_t stableIndex)
{
    const AssemblyInteriorResult validated = ValidateInteriorEntities(scene);
    return validated.Succeeded()
        ? m_interior.ActivateInteraction(stableIndex)
        : validated;
}

AssemblyInteriorResult AssemblyRuntimeHost::ActivateInteraction(
    Scene& scene,
    std::string_view id)
{
    const AssemblyInteriorResult validated = ValidateInteriorEntities(scene);
    return validated.Succeeded()
        ? m_interior.ActivateInteraction(id)
        : validated;
}

AssemblyInteriorResult AssemblyRuntimeHost::ActivateNearestInteraction(
    Scene& scene,
    const AssemblyInteractionQuery& query)
{
    const AssemblyInteriorResult validated = ValidateInteriorEntities(scene);
    return validated.Succeeded()
        ? m_interior.ActivateNearest(query)
        : validated;
}

AssemblyInteriorResult AssemblyRuntimeHost::AdvanceInterior(
    Scene& scene,
    double dt,
    const AssemblyInteriorConfig& config)
{
    const AssemblyInteriorResult validated = ValidateInteriorEntities(scene);
    if (!validated.Succeeded())
        return validated;
    const AssemblyInteriorResult advanced = m_interior.Advance(dt, config);
    if (!advanced.Succeeded() || !advanced.changed)
        return advanced;
    const AssemblyInteriorResult applied = ApplyInteriorTransforms(scene);
    if (!applied.Succeeded())
        return applied;
    return advanced;
}

AssemblyInteriorSnapshot AssemblyRuntimeHost::CaptureInteriorSnapshot() const
{
    return m_interior.CaptureSnapshot();
}

AssemblyInteriorResult AssemblyRuntimeHost::ApplyInteriorSnapshot(
    Scene& scene,
    const AssemblyInteriorSnapshot& snapshot)
{
    const AssemblyInteriorResult validated = ValidateInteriorEntities(scene);
    if (!validated.Succeeded())
        return validated;
    const AssemblyInteriorResult applied = m_interior.ApplySnapshot(snapshot);
    if (!applied.Succeeded() || !applied.changed)
        return applied;
    const AssemblyInteriorResult transformed = ApplyInteriorTransforms(scene);
    if (!transformed.Succeeded())
        return transformed;
    return applied;
}

bool AssemblyRuntimeHost::Shutdown(
    Scene& scene,
    render::D3D12Device& device,
    render::Renderer& renderer) noexcept
{
    bool ok = true;
    m_interior.Shutdown();
    if (m_instance && m_instance->IsAlive())
    {
        RegistryAssemblyEntityTarget target(scene.GetRegistry());
        ok = DestroyAssemblyInstance(target, *m_instance) ==
            AssemblyInstantiationStatus::Success;
    }
    ReleaseState(scene, device, renderer);
    return ok;
}

void AssemblyRuntimeHost::ReleaseState(
    Scene& scene,
    render::D3D12Device& device,
    render::Renderer& renderer) noexcept
{
    m_interior.Shutdown();
    m_instance.reset();
    m_plan.reset();
    m_catalog.reset();
    m_owners.reset();
    for (auto it = m_models.rbegin(); it != m_models.rend(); ++it)
        ReleaseLoadedModelResources(*it, scene, device, renderer);
    m_models.clear();
    m_assembly.reset();
    m_manifest = {};
    m_pending = false;
}

} // namespace scene
