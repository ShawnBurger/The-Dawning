#pragma once

#include "cooked_assembly.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace asset
{

enum class AssemblyResourceKind : uint8_t
{
    Visual = 1,
    Collision = 2,
    NavigationMesh = 3,
    WalkableSurface = 4
};

const char* AssemblyResourceKindName(AssemblyResourceKind kind);

struct AssemblyCatalogIdentity
{
    AssemblyResourceKind kind = AssemblyResourceKind::Visual;
    uint64_t value = 0;
    uint32_t generation = 0;
    Sha256Digest contentSha256;

    bool IsValid() const;
    bool operator==(const AssemblyCatalogIdentity&) const = default;
};

enum class AssemblyCatalogLookupStatus : uint8_t
{
    Found,
    NotFound,
    Stale,
    Error
};

struct AssemblyCatalogLookup
{
    AssemblyCatalogLookupStatus status = AssemblyCatalogLookupStatus::Error;
    AssemblyCatalogIdentity identity;
    std::string error;

    bool Found() const { return status == AssemblyCatalogLookupStatus::Found; }
};

class AssemblyResourceCatalog
{
public:
    virtual ~AssemblyResourceCatalog() = default;

    // Query-only boundary. Implementations resolve an already indexed catalog;
    // they must not load files, upload resources, or mutate scene state here.
    virtual AssemblyCatalogLookup Resolve(
        AssemblyResourceKind kind,
        std::string_view locator) const = 0;
};

template <AssemblyResourceKind Kind>
struct TypedAssemblyResourceIdentity
{
    static constexpr AssemblyResourceKind kKind = Kind;

    uint64_t value = 0;
    uint32_t generation = 0;
    Sha256Digest contentSha256;

    bool IsValid() const
    {
        if (value == 0)
            return false;
        for (const uint8_t byte : contentSha256.bytes)
        {
            if (byte != 0)
                return true;
        }
        return false;
    }

    bool operator==(const TypedAssemblyResourceIdentity&) const = default;
};

using AssemblyVisualIdentity =
    TypedAssemblyResourceIdentity<AssemblyResourceKind::Visual>;
using AssemblyCollisionIdentity =
    TypedAssemblyResourceIdentity<AssemblyResourceKind::Collision>;
using AssemblyNavigationMeshIdentity =
    TypedAssemblyResourceIdentity<AssemblyResourceKind::NavigationMesh>;
using AssemblyWalkableSurfaceIdentity =
    TypedAssemblyResourceIdentity<AssemblyResourceKind::WalkableSurface>;

struct ResolvedAssemblyModuleResources
{
    AssemblyVisualIdentity visual;
    AssemblyCollisionIdentity collision;
    std::vector<AssemblyVisualIdentity> lods;
};

struct ResolvedAssemblyZoneResources
{
    AssemblyNavigationMeshIdentity navigationMesh;
    AssemblyWalkableSurfaceIdentity walkableSurface;
};

struct ResolvedAssemblyMovingPartResources
{
    AssemblyVisualIdentity visual;
};

struct ResolvedAssemblyResources
{
    // Retaining the exact immutable source keeps manifest provenance and every
    // cooked stable index attached to the bindings without copying graph data.
    std::shared_ptr<const CookedAssembly> assembly;
    Sha256Digest sourceManifestSha256;
    std::vector<ResolvedAssemblyModuleResources> modules;
    std::vector<ResolvedAssemblyZoneResources> zones;
    std::vector<ResolvedAssemblyMovingPartResources> movingParts;
};

enum class AssemblyResourceResolutionStatus : uint8_t
{
    Success,
    InvalidArgument,
    InvalidAssembly,
    ResourceLimitExceeded,
    CatalogNotFound,
    CatalogStale,
    CatalogError,
    InvalidCatalogIdentity,
    CatalogKindMismatch,
    IdentityConflict,
    AllocationFailure,
    InternalError
};

const char* AssemblyResourceResolutionStatusName(
    AssemblyResourceResolutionStatus status);

struct AssemblyResourceResolutionLimits
{
    uint64_t maxBindings = 1'000'000ull;
    uint64_t maxUniqueRequests = 500'000ull;
    uint32_t maxLocatorBytes = 1024u * 1024u;
    uint32_t maxCatalogErrorBytes = 1024u;
};

struct AssemblyResourceResolutionResult
{
    AssemblyResourceResolutionStatus status =
        AssemblyResourceResolutionStatus::InvalidArgument;
    std::shared_ptr<const ResolvedAssemblyResources> resources;
    std::string error;

    bool Succeeded() const
    {
        return status == AssemblyResourceResolutionStatus::Success &&
               resources != nullptr;
    }
};

AssemblyResourceResolutionResult ResolveAssemblyResources(
    std::shared_ptr<const CookedAssembly> assembly,
    const AssemblyResourceCatalog& catalog,
    const AssemblyResourceResolutionLimits& limits = {});

} // namespace asset
