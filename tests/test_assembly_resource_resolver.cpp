#include "test_framework.h"
#include "asset/assembly_resource_resolver.h"

#include <cstddef>
#include <algorithm>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

using CatalogKey = std::pair<asset::AssemblyResourceKind, std::string>;

asset::Sha256Digest HashText(std::string_view text)
{
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return asset::ComputeSha256(std::span<const std::byte>(data, text.size()));
}

asset::AssemblyCatalogLookup Found(
    asset::AssemblyResourceKind kind,
    uint64_t value,
    std::string_view content,
    uint32_t generation = 0)
{
    asset::AssemblyCatalogLookup result;
    result.status = asset::AssemblyCatalogLookupStatus::Found;
    result.identity.kind = kind;
    result.identity.value = value;
    result.identity.generation = generation;
    result.identity.contentSha256 = HashText(content);
    return result;
}

class FakeCatalog final : public asset::AssemblyResourceCatalog
{
public:
    enum class ThrowMode
    {
        None,
        Runtime,
        Allocation,
        Unknown
    };

    asset::AssemblyCatalogLookup Resolve(
        asset::AssemblyResourceKind kind,
        std::string_view locator) const override
    {
        calls.emplace_back(kind, locator);
        if (throwMode == ThrowMode::Runtime)
            throw std::runtime_error("catalog test failure");
        if (throwMode == ThrowMode::Allocation)
            throw std::bad_alloc();
        if (throwMode == ThrowMode::Unknown)
            throw 17;

        const auto it = entries.find(CatalogKey{ kind, std::string(locator) });
        if (it != entries.end())
            return it->second;

        asset::AssemblyCatalogLookup result;
        result.status = asset::AssemblyCatalogLookupStatus::NotFound;
        result.error = "missing test entry";
        return result;
    }

    void Add(
        asset::AssemblyResourceKind kind,
        std::string_view locator,
        asset::AssemblyCatalogLookup lookup)
    {
        entries.emplace(CatalogKey{ kind, std::string(locator) }, std::move(lookup));
    }

    std::map<CatalogKey, asset::AssemblyCatalogLookup> entries;
    mutable std::vector<CatalogKey> calls;
    ThrowMode throwMode = ThrowMode::None;
};

std::shared_ptr<asset::CookedAssembly> MakeAssembly()
{
    auto assembly = std::make_shared<asset::CookedAssembly>();
    assembly->schemaVersion = 1;
    assembly->assetId = "ship.test.resolution";
    assembly->sourceManifestSha256 = HashText("source manifest");
    assembly->provenance.push_back(asset::AssemblyProvenance{
        "source", "Meshy", HashText("request")
    });

    asset::AssemblyModule hull;
    hull.id = "hull";
    hull.provenanceIndex = 0;
    hull.visualSource = "shared://surface";
    hull.collisionSource = "collision://hull";
    hull.lods.push_back(asset::AssemblyLod{ 1, "visual://lod2", 500.0 });
    assembly->modules.push_back(hull);

    asset::AssemblyModule cockpit;
    cockpit.id = "cockpit";
    cockpit.provenanceIndex = 0;
    cockpit.visualSource = "visual://cockpit";
    cockpit.collisionSource = "shared://surface";
    cockpit.lods.push_back(asset::AssemblyLod{ 1, "visual://lod2", 500.0 });
    assembly->modules.push_back(cockpit);

    asset::AssemblyZone zone;
    zone.id = "cockpit";
    zone.moduleIndex = 1;
    zone.navmeshSource = "nav://cockpit";
    zone.walkableSurface = "shared://surface";
    assembly->zones.push_back(zone);

    asset::AssemblyMovingPart part;
    part.id = "hatch";
    part.moduleIndex = 1;
    part.visualSource = "visual://cockpit";
    assembly->movingParts.push_back(part);
    return assembly;
}

FakeCatalog MakeCatalog()
{
    FakeCatalog catalog;
    catalog.Add(
        asset::AssemblyResourceKind::Visual,
        "shared://surface",
        Found(asset::AssemblyResourceKind::Visual, 11, "visual shared", 2));
    catalog.Add(
        asset::AssemblyResourceKind::Visual,
        "visual://cockpit",
        Found(asset::AssemblyResourceKind::Visual, 12, "visual cockpit", 3));
    catalog.Add(
        asset::AssemblyResourceKind::Visual,
        "visual://lod2",
        Found(asset::AssemblyResourceKind::Visual, 13, "visual lod2", 4));
    catalog.Add(
        asset::AssemblyResourceKind::Collision,
        "collision://hull",
        Found(asset::AssemblyResourceKind::Collision, 21, "collision hull", 5));
    catalog.Add(
        asset::AssemblyResourceKind::Collision,
        "shared://surface",
        Found(asset::AssemblyResourceKind::Collision, 22, "collision shared", 6));
    catalog.Add(
        asset::AssemblyResourceKind::NavigationMesh,
        "nav://cockpit",
        Found(asset::AssemblyResourceKind::NavigationMesh, 31, "navigation", 7));
    catalog.Add(
        asset::AssemblyResourceKind::WalkableSurface,
        "shared://surface",
        Found(asset::AssemblyResourceKind::WalkableSurface, 41, "walkable", 8));
    return catalog;
}

void CheckFailure(
    const asset::AssemblyResourceResolutionResult& result,
    asset::AssemblyResourceResolutionStatus expected)
{
    CHECK_FALSE(result.Succeeded());
    CHECK_EQ(result.status, expected);
    CHECK(result.resources == nullptr);
}

} // namespace

static_assert(!std::is_same_v<
              asset::AssemblyVisualIdentity,
              asset::AssemblyCollisionIdentity>);
static_assert(!std::is_same_v<
              asset::AssemblyNavigationMeshIdentity,
              asset::AssemblyWalkableSurfaceIdentity>);

TEST_CASE(AssemblyResourceResolver_DeduplicatesCanonicallyAndPreservesStableIndices)
{
    const auto assembly = MakeAssembly();
    FakeCatalog catalog = MakeCatalog();

    const auto result = asset::ResolveAssemblyResources(assembly, catalog);

    CHECK(result.Succeeded());
    CHECK(result.resources != nullptr);
    CHECK(result.resources->assembly == assembly);
    CHECK_EQ(result.resources->sourceManifestSha256, assembly->sourceManifestSha256);
    CHECK_EQ(result.resources->modules.size(), (size_t)2);
    CHECK_EQ(result.resources->zones.size(), (size_t)1);
    CHECK_EQ(result.resources->movingParts.size(), (size_t)1);

    CHECK_EQ(result.resources->modules[0].visual.value, (uint64_t)11);
    CHECK_EQ(result.resources->modules[0].collision.value, (uint64_t)21);
    CHECK_EQ(result.resources->modules[0].lods.size(), (size_t)1);
    CHECK_EQ(result.resources->modules[0].lods[0].value, (uint64_t)13);
    CHECK_EQ(result.resources->modules[1].visual.value, (uint64_t)12);
    CHECK_EQ(result.resources->modules[1].collision.value, (uint64_t)22);
    CHECK_EQ(result.resources->modules[1].lods[0].value, (uint64_t)13);
    CHECK_EQ(result.resources->zones[0].navigationMesh.value, (uint64_t)31);
    CHECK_EQ(result.resources->zones[0].walkableSurface.value, (uint64_t)41);
    CHECK_EQ(result.resources->movingParts[0].visual.value, (uint64_t)12);

    const std::vector<CatalogKey> expected = {
        { asset::AssemblyResourceKind::Visual, "shared://surface" },
        { asset::AssemblyResourceKind::Visual, "visual://cockpit" },
        { asset::AssemblyResourceKind::Visual, "visual://lod2" },
        { asset::AssemblyResourceKind::Collision, "collision://hull" },
        { asset::AssemblyResourceKind::Collision, "shared://surface" },
        { asset::AssemblyResourceKind::NavigationMesh, "nav://cockpit" },
        { asset::AssemblyResourceKind::WalkableSurface, "shared://surface" }
    };
    CHECK_EQ(catalog.calls, expected);

    auto reordered = MakeAssembly();
    std::reverse(reordered->modules.begin(), reordered->modules.end());
    FakeCatalog reorderedCatalog = MakeCatalog();
    const auto reorderedResult =
        asset::ResolveAssemblyResources(reordered, reorderedCatalog);
    CHECK(reorderedResult.Succeeded());
    CHECK_EQ(reorderedCatalog.calls, expected);
    CHECK_EQ(reorderedResult.resources->modules[0].visual.value, (uint64_t)12);
    CHECK_EQ(reorderedResult.resources->modules[1].visual.value, (uint64_t)11);
}

TEST_CASE(AssemblyResourceResolver_AllowsSameKindAliasesOnlyForIdenticalContent)
{
    const auto assembly = MakeAssembly();
    FakeCatalog catalog = MakeCatalog();
    catalog.entries[{
        asset::AssemblyResourceKind::Visual, "visual://cockpit"
    }] = Found(asset::AssemblyResourceKind::Visual, 11, "visual shared", 2);

    const auto aliased = asset::ResolveAssemblyResources(assembly, catalog);
    CHECK(aliased.Succeeded());
    CHECK_EQ(aliased.resources->modules[0].visual.value, (uint64_t)11);
    CHECK_EQ(aliased.resources->modules[1].visual.value, (uint64_t)11);

    catalog = MakeCatalog();
    catalog.entries[{
        asset::AssemblyResourceKind::Visual, "visual://cockpit"
    }] = Found(asset::AssemblyResourceKind::Visual, 11, "different bytes", 2);
    const auto conflicting = asset::ResolveAssemblyResources(assembly, catalog);
    CheckFailure(
        conflicting,
        asset::AssemblyResourceResolutionStatus::IdentityConflict);
}

TEST_CASE(AssemblyResourceResolver_RejectsMissingStaleAndFailedCatalogQueries)
{
    const auto assembly = MakeAssembly();

    FakeCatalog missing = MakeCatalog();
    missing.entries.erase({
        asset::AssemblyResourceKind::Collision, "collision://hull"
    });
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, missing),
        asset::AssemblyResourceResolutionStatus::CatalogNotFound);

    FakeCatalog stale = MakeCatalog();
    stale.entries[{
        asset::AssemblyResourceKind::Collision, "collision://hull"
    }].status = asset::AssemblyCatalogLookupStatus::Stale;
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, stale),
        asset::AssemblyResourceResolutionStatus::CatalogStale);

    FakeCatalog failed = MakeCatalog();
    failed.entries[{
        asset::AssemblyResourceKind::Collision, "collision://hull"
    }].status = asset::AssemblyCatalogLookupStatus::Error;
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, failed),
        asset::AssemblyResourceResolutionStatus::CatalogError);

    FakeCatalog unknownStatus = MakeCatalog();
    unknownStatus.entries[{
        asset::AssemblyResourceKind::Visual, "shared://surface"
    }].status = static_cast<asset::AssemblyCatalogLookupStatus>(127);
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, unknownStatus),
        asset::AssemblyResourceResolutionStatus::CatalogError);
}

TEST_CASE(AssemblyResourceResolver_RejectsMalformedAndWrongKindIdentities)
{
    const auto assembly = MakeAssembly();

    FakeCatalog zeroValue = MakeCatalog();
    zeroValue.entries[{
        asset::AssemblyResourceKind::Visual, "shared://surface"
    }].identity.value = 0;
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, zeroValue),
        asset::AssemblyResourceResolutionStatus::InvalidCatalogIdentity);

    FakeCatalog zeroDigest = MakeCatalog();
    zeroDigest.entries[{
        asset::AssemblyResourceKind::Visual, "shared://surface"
    }].identity.contentSha256 = {};
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, zeroDigest),
        asset::AssemblyResourceResolutionStatus::InvalidCatalogIdentity);

    FakeCatalog wrongKind = MakeCatalog();
    wrongKind.entries[{
        asset::AssemblyResourceKind::Visual, "shared://surface"
    }].identity.kind = asset::AssemblyResourceKind::Collision;
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, wrongKind),
        asset::AssemblyResourceResolutionStatus::CatalogKindMismatch);

    FakeCatalog unknownKind = MakeCatalog();
    unknownKind.entries[{
        asset::AssemblyResourceKind::Visual, "shared://surface"
    }].identity.kind = static_cast<asset::AssemblyResourceKind>(127);
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, unknownKind),
        asset::AssemblyResourceResolutionStatus::InvalidCatalogIdentity);
}

TEST_CASE(AssemblyResourceResolver_ContainsCatalogExceptionsWithoutPublication)
{
    const auto assembly = MakeAssembly();

    FakeCatalog runtimeFailure = MakeCatalog();
    runtimeFailure.throwMode = FakeCatalog::ThrowMode::Runtime;
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, runtimeFailure),
        asset::AssemblyResourceResolutionStatus::CatalogError);

    FakeCatalog allocationFailure = MakeCatalog();
    allocationFailure.throwMode = FakeCatalog::ThrowMode::Allocation;
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, allocationFailure),
        asset::AssemblyResourceResolutionStatus::AllocationFailure);

    FakeCatalog unknownFailure = MakeCatalog();
    unknownFailure.throwMode = FakeCatalog::ThrowMode::Unknown;
    CheckFailure(
        asset::ResolveAssemblyResources(assembly, unknownFailure),
        asset::AssemblyResourceResolutionStatus::CatalogError);
}

TEST_CASE(AssemblyResourceResolver_BoundsUntrustedCatalogDiagnostics)
{
    const auto assembly = MakeAssembly();
    FakeCatalog catalog = MakeCatalog();
    auto& missing = catalog.entries[{
        asset::AssemblyResourceKind::Visual, "shared://surface"
    }];
    missing.status = asset::AssemblyCatalogLookupStatus::NotFound;
    missing.error.assign(100'000, 'x');

    asset::AssemblyResourceResolutionLimits limits;
    limits.maxCatalogErrorBytes = 7;
    const auto result =
        asset::ResolveAssemblyResources(assembly, catalog, limits);

    CheckFailure(
        result,
        asset::AssemblyResourceResolutionStatus::CatalogNotFound);
    CHECK(result.error.size() < (size_t)256);
    CHECK(result.error.ends_with("xxxxxxx"));

    catalog = MakeCatalog();
    auto& utf8 = catalog.entries[{
        asset::AssemblyResourceKind::Visual, "shared://surface"
    }];
    utf8.status = asset::AssemblyCatalogLookupStatus::NotFound;
    utf8.error = "aaaaaa";
    const unsigned char utf8Lead = 0xc3u;
    const unsigned char utf8Continuation = 0xa9u;
    utf8.error.push_back(static_cast<char>(utf8Lead));
    utf8.error.push_back(static_cast<char>(utf8Continuation));
    const auto utf8Result =
        asset::ResolveAssemblyResources(assembly, catalog, limits);
    CheckFailure(
        utf8Result,
        asset::AssemblyResourceResolutionStatus::CatalogNotFound);
    CHECK(utf8Result.error.ends_with("aaaaaa"));

    catalog = MakeCatalog();
    auto& unsafe = catalog.entries[{
        asset::AssemblyResourceKind::Visual, "shared://surface"
    }];
    unsafe.status = asset::AssemblyCatalogLookupStatus::NotFound;
    unsafe.error = "unsafe\ndetail";
    limits.maxCatalogErrorBytes = 1024;
    const auto unsafeResult =
        asset::ResolveAssemblyResources(assembly, catalog, limits);
    CheckFailure(
        unsafeResult,
        asset::AssemblyResourceResolutionStatus::CatalogNotFound);
    CHECK(unsafeResult.error.find(": ") == std::string::npos);
}

TEST_CASE(AssemblyResourceResolver_RejectsUnsafeLocatorsAndResourceAmplification)
{
    FakeCatalog catalog = MakeCatalog();

    CheckFailure(
        asset::ResolveAssemblyResources({}, catalog),
        asset::AssemblyResourceResolutionStatus::InvalidArgument);

    auto empty = MakeAssembly();
    empty->modules[0].visualSource.clear();
    CheckFailure(
        asset::ResolveAssemblyResources(empty, catalog),
        asset::AssemblyResourceResolutionStatus::InvalidAssembly);

    auto control = MakeAssembly();
    control->modules[0].visualSource = "visual://bad\nlocator";
    CheckFailure(
        asset::ResolveAssemblyResources(control, catalog),
        asset::AssemblyResourceResolutionStatus::InvalidAssembly);

    auto invalidUtf8 = MakeAssembly();
    const unsigned char invalidByte = 0xffu;
    invalidUtf8->modules[0].visualSource = "visual://";
    invalidUtf8->modules[0].visualSource.push_back(static_cast<char>(invalidByte));
    CheckFailure(
        asset::ResolveAssemblyResources(invalidUtf8, catalog),
        asset::AssemblyResourceResolutionStatus::InvalidAssembly);

    asset::AssemblyResourceResolutionLimits limits;
    limits.maxBindings = 2;
    CheckFailure(
        asset::ResolveAssemblyResources(MakeAssembly(), catalog, limits),
        asset::AssemblyResourceResolutionStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxUniqueRequests = 2;
    CheckFailure(
        asset::ResolveAssemblyResources(MakeAssembly(), catalog, limits),
        asset::AssemblyResourceResolutionStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxLocatorBytes = 5;
    CheckFailure(
        asset::ResolveAssemblyResources(MakeAssembly(), catalog, limits),
        asset::AssemblyResourceResolutionStatus::InvalidAssembly);

    limits = {};
    limits.maxBindings = 0;
    CheckFailure(
        asset::ResolveAssemblyResources(MakeAssembly(), catalog, limits),
        asset::AssemblyResourceResolutionStatus::InvalidArgument);

    limits = {};
    limits.maxCatalogErrorBytes = 64u * 1024u + 1u;
    CheckFailure(
        asset::ResolveAssemblyResources(MakeAssembly(), catalog, limits),
        asset::AssemblyResourceResolutionStatus::InvalidArgument);
}

TEST_CASE(AssemblyResourceResolver_StatusAndIdentitySurfacesAreExplicit)
{
    const auto identity = Found(
        asset::AssemblyResourceKind::Visual, 7, "identity").identity;
    CHECK(identity.IsValid());
    CHECK_EQ(std::string(asset::AssemblyResourceKindName(identity.kind)), "visual");
    CHECK_EQ(
        std::string(asset::AssemblyResourceResolutionStatusName(
            asset::AssemblyResourceResolutionStatus::CatalogStale)),
        "catalog stale");
    CHECK_EQ(
        std::string(asset::AssemblyResourceKindName(
            static_cast<asset::AssemblyResourceKind>(127))),
        "unknown");
}
