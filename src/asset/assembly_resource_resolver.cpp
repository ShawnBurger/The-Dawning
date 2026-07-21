#include "assembly_resource_resolver.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <new>
#include <limits>
#include <utility>

namespace asset
{
namespace
{

struct Request
{
    AssemblyResourceKind kind = AssemblyResourceKind::Visual;
    std::string_view locator;
};

struct AcceptedRequest
{
    Request request;
    AssemblyCatalogIdentity identity;
};

struct IdentityKey
{
    AssemblyResourceKind kind = AssemblyResourceKind::Visual;
    uint64_t value = 0;
    uint32_t generation = 0;

    bool operator<(const IdentityKey& other) const
    {
        if (kind != other.kind)
            return static_cast<uint8_t>(kind) < static_cast<uint8_t>(other.kind);
        if (value != other.value)
            return value < other.value;
        return generation < other.generation;
    }
};

bool IsKnownKind(AssemblyResourceKind kind)
{
    switch (kind)
    {
    case AssemblyResourceKind::Visual:
    case AssemblyResourceKind::Collision:
    case AssemblyResourceKind::NavigationMesh:
    case AssemblyResourceKind::WalkableSurface:
        return true;
    }
    return false;
}

bool IsNonZero(const Sha256Digest& digest)
{
    return std::any_of(digest.bytes.begin(), digest.bytes.end(), [](uint8_t byte) {
        return byte != 0;
    });
}

bool IsValidUtf8(std::string_view text)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    size_t index = 0;
    while (index < text.size())
    {
        const uint8_t first = bytes[index++];
        if (first <= 0x7f)
            continue;

        uint32_t codePoint = 0;
        uint32_t continuationCount = 0;
        if (first >= 0xc2 && first <= 0xdf)
        {
            codePoint = first & 0x1f;
            continuationCount = 1;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            codePoint = first & 0x0f;
            continuationCount = 2;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            codePoint = first & 0x07;
            continuationCount = 3;
        }
        else
        {
            return false;
        }

        if (continuationCount > text.size() - index)
            return false;
        for (uint32_t i = 0; i < continuationCount; ++i)
        {
            const uint8_t next = bytes[index++];
            if ((next & 0xc0) != 0x80)
                return false;
            codePoint = (codePoint << 6) | (next & 0x3f);
        }

        if ((continuationCount == 2 && codePoint < 0x800) ||
            (continuationCount == 3 && codePoint < 0x10000) ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff) ||
            codePoint > 0x10ffff)
        {
            return false;
        }
    }
    return true;
}

bool IsSafeLocator(std::string_view locator, uint32_t maxBytes)
{
    if (locator.empty() || locator.size() > maxBytes || !IsValidUtf8(locator))
        return false;
    return std::none_of(locator.begin(), locator.end(), [](unsigned char value) {
        return value < 0x20 || value == 0x7f;
    });
}

int CompareLocator(std::string_view left, std::string_view right)
{
    const size_t common = (std::min)(left.size(), right.size());
    if (common > 0)
    {
        const int comparison = std::memcmp(left.data(), right.data(), common);
        if (comparison != 0)
            return comparison;
    }
    if (left.size() < right.size())
        return -1;
    if (left.size() > right.size())
        return 1;
    return 0;
}

bool RequestLess(const Request& left, const Request& right)
{
    if (left.kind != right.kind)
    {
        return static_cast<uint8_t>(left.kind) <
               static_cast<uint8_t>(right.kind);
    }
    return CompareLocator(left.locator, right.locator) < 0;
}

bool RequestEqual(const Request& left, const Request& right)
{
    return left.kind == right.kind && left.locator == right.locator;
}

AssemblyResourceResolutionResult Failure(
    AssemblyResourceResolutionStatus status,
    std::string error)
{
    AssemblyResourceResolutionResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

std::string LocatorPreview(std::string_view locator)
{
    constexpr size_t kMaxPreviewBytes = 128;
    if (locator.size() <= kMaxPreviewBytes)
        return std::string(locator);
    size_t prefixBytes = kMaxPreviewBytes;
    while (prefixBytes > 0 &&
           (static_cast<unsigned char>(locator[prefixBytes]) & 0xc0u) == 0x80u)
    {
        --prefixBytes;
    }
    std::string result(locator.substr(0, prefixBytes));
    result += "...";
    return result;
}

std::string ExternalErrorPreview(std::string_view error, uint32_t maxBytes)
{
    if (error.empty() || maxBytes == 0)
        return {};
    size_t prefixBytes = (std::min)(error.size(), static_cast<size_t>(maxBytes));
    while (prefixBytes > 0 && prefixBytes < error.size() &&
           (static_cast<unsigned char>(error[prefixBytes]) & 0xc0u) == 0x80u)
    {
        --prefixBytes;
    }
    const std::string_view prefix = error.substr(0, prefixBytes);
    if (!IsValidUtf8(prefix) ||
        std::any_of(prefix.begin(), prefix.end(), [](unsigned char value) {
            return value < 0x20 || value == 0x7f;
        }))
    {
        return {};
    }
    return std::string(prefix);
}

std::string LookupFailureMessage(
    std::string_view prefix,
    const Request& request,
    std::string_view catalogError,
    uint32_t maxCatalogErrorBytes)
{
    std::string result(prefix);
    result += " ";
    result += AssemblyResourceKindName(request.kind);
    result += " resource '";
    result += LocatorPreview(request.locator);
    result += "'";
    const std::string detail =
        ExternalErrorPreview(catalogError, maxCatalogErrorBytes);
    if (!detail.empty())
    {
        result += ": ";
        result += detail;
    }
    return result;
}

bool AddBindingCount(uint64_t& total, uint64_t amount, uint64_t maximum)
{
    if (amount > maximum || total > maximum - amount)
        return false;
    total += amount;
    return true;
}

template <AssemblyResourceKind Kind>
TypedAssemblyResourceIdentity<Kind> ToTyped(
    const AssemblyCatalogIdentity& identity)
{
    TypedAssemblyResourceIdentity<Kind> result;
    result.value = identity.value;
    result.generation = identity.generation;
    result.contentSha256 = identity.contentSha256;
    return result;
}

const AssemblyCatalogIdentity* FindAccepted(
    const std::vector<AcceptedRequest>& accepted,
    AssemblyResourceKind kind,
    std::string_view locator)
{
    const Request key{ kind, locator };
    const auto it = std::lower_bound(
        accepted.begin(), accepted.end(), key,
        [](const AcceptedRequest& value, const Request& request) {
            return RequestLess(value.request, request);
        });
    if (it == accepted.end() || !RequestEqual(it->request, key))
        return nullptr;
    return &it->identity;
}

AssemblyResourceResolutionResult ResolveImpl(
    const std::shared_ptr<const CookedAssembly>& assembly,
    const AssemblyResourceCatalog& catalog,
    const AssemblyResourceResolutionLimits& limits)
{
    if (!assembly)
    {
        return Failure(
            AssemblyResourceResolutionStatus::InvalidArgument,
            "cooked assembly is null");
    }
    if (limits.maxBindings == 0 || limits.maxUniqueRequests == 0 ||
        limits.maxLocatorBytes == 0 || limits.maxCatalogErrorBytes > 64u * 1024u)
    {
        return Failure(
            AssemblyResourceResolutionStatus::InvalidArgument,
            "resource resolution limits are zero or outside hard diagnostic bounds");
    }

    uint64_t bindingCount = 0;
    for (const AssemblyModule& module : assembly->modules)
    {
        if (!AddBindingCount(bindingCount, 2, limits.maxBindings) ||
            !AddBindingCount(
                bindingCount,
                static_cast<uint64_t>(module.lods.size()),
                limits.maxBindings))
        {
            return Failure(
                AssemblyResourceResolutionStatus::ResourceLimitExceeded,
                "assembly resource binding count exceeds configured limit");
        }
    }
    if (!AddBindingCount(
            bindingCount,
            static_cast<uint64_t>(assembly->zones.size()),
            limits.maxBindings) ||
        !AddBindingCount(
            bindingCount,
            static_cast<uint64_t>(assembly->zones.size()),
            limits.maxBindings) ||
        !AddBindingCount(
            bindingCount,
            static_cast<uint64_t>(assembly->movingParts.size()),
            limits.maxBindings))
    {
        return Failure(
            AssemblyResourceResolutionStatus::ResourceLimitExceeded,
            "assembly resource binding count exceeds configured limit");
    }
    if (bindingCount > (std::numeric_limits<size_t>::max)())
    {
        return Failure(
            AssemblyResourceResolutionStatus::ResourceLimitExceeded,
            "assembly resource binding count exceeds addressable memory");
    }

    std::vector<Request> requests;
    requests.reserve(static_cast<size_t>(bindingCount));
    auto append = [&](AssemblyResourceKind kind, std::string_view locator) {
        if (!IsSafeLocator(locator, limits.maxLocatorBytes))
            return false;
        requests.push_back(Request{ kind, locator });
        return true;
    };

    for (const AssemblyModule& module : assembly->modules)
    {
        if (!append(AssemblyResourceKind::Visual, module.visualSource) ||
            !append(AssemblyResourceKind::Collision, module.collisionSource))
        {
            return Failure(
                AssemblyResourceResolutionStatus::InvalidAssembly,
                "module contains an empty, oversized, unsafe, or invalid UTF-8 locator");
        }
        for (const AssemblyLod& lod : module.lods)
        {
            if (!append(AssemblyResourceKind::Visual, lod.source))
            {
                return Failure(
                    AssemblyResourceResolutionStatus::InvalidAssembly,
                    "module LOD contains an empty, oversized, unsafe, or invalid UTF-8 locator");
            }
        }
    }
    for (const AssemblyZone& zone : assembly->zones)
    {
        if (!append(AssemblyResourceKind::NavigationMesh, zone.navmeshSource) ||
            !append(AssemblyResourceKind::WalkableSurface, zone.walkableSurface))
        {
            return Failure(
                AssemblyResourceResolutionStatus::InvalidAssembly,
                "zone contains an empty, oversized, unsafe, or invalid UTF-8 locator");
        }
    }
    for (const AssemblyMovingPart& part : assembly->movingParts)
    {
        if (!append(AssemblyResourceKind::Visual, part.visualSource))
        {
            return Failure(
                AssemblyResourceResolutionStatus::InvalidAssembly,
                "moving part contains an empty, oversized, unsafe, or invalid UTF-8 locator");
        }
    }

    std::sort(requests.begin(), requests.end(), RequestLess);
    requests.erase(
        std::unique(requests.begin(), requests.end(), RequestEqual),
        requests.end());
    if (requests.size() > limits.maxUniqueRequests)
    {
        return Failure(
            AssemblyResourceResolutionStatus::ResourceLimitExceeded,
            "unique assembly resource request count exceeds configured limit");
    }

    std::vector<AcceptedRequest> accepted;
    accepted.reserve(requests.size());
    std::map<IdentityKey, Sha256Digest> identityDigests;
    for (const Request& request : requests)
    {
        AssemblyCatalogLookup lookup;
        try
        {
            lookup = catalog.Resolve(request.kind, request.locator);
        }
        catch (const std::bad_alloc&)
        {
            return Failure(
                AssemblyResourceResolutionStatus::AllocationFailure,
                "catalog allocation failed during resource resolution");
        }
        catch (const std::exception& exception)
        {
            return Failure(
                AssemblyResourceResolutionStatus::CatalogError,
                LookupFailureMessage(
                    "catalog threw while resolving",
                    request,
                    exception.what(),
                    limits.maxCatalogErrorBytes));
        }
        catch (...)
        {
            return Failure(
                AssemblyResourceResolutionStatus::CatalogError,
                LookupFailureMessage(
                    "catalog threw while resolving",
                    request,
                    {},
                    limits.maxCatalogErrorBytes));
        }

        switch (lookup.status)
        {
        case AssemblyCatalogLookupStatus::NotFound:
            return Failure(
                AssemblyResourceResolutionStatus::CatalogNotFound,
                LookupFailureMessage(
                    "catalog did not contain",
                    request,
                    lookup.error,
                    limits.maxCatalogErrorBytes));
        case AssemblyCatalogLookupStatus::Stale:
            return Failure(
                AssemblyResourceResolutionStatus::CatalogStale,
                LookupFailureMessage(
                    "catalog identity was stale for",
                    request,
                    lookup.error,
                    limits.maxCatalogErrorBytes));
        case AssemblyCatalogLookupStatus::Error:
            return Failure(
                AssemblyResourceResolutionStatus::CatalogError,
                LookupFailureMessage(
                    "catalog failed to resolve",
                    request,
                    lookup.error,
                    limits.maxCatalogErrorBytes));
        case AssemblyCatalogLookupStatus::Found:
            break;
        default:
            return Failure(
                AssemblyResourceResolutionStatus::CatalogError,
                LookupFailureMessage(
                    "catalog returned an unknown status for",
                    request,
                    {},
                    limits.maxCatalogErrorBytes));
        }

        if (!IsKnownKind(lookup.identity.kind))
        {
            return Failure(
                AssemblyResourceResolutionStatus::InvalidCatalogIdentity,
                LookupFailureMessage(
                    "catalog returned an invalid kind for",
                    request,
                    {},
                    limits.maxCatalogErrorBytes));
        }
        if (lookup.identity.kind != request.kind)
        {
            return Failure(
                AssemblyResourceResolutionStatus::CatalogKindMismatch,
                LookupFailureMessage(
                    "catalog returned the wrong kind for",
                    request,
                    {},
                    limits.maxCatalogErrorBytes));
        }
        if (!lookup.identity.IsValid())
        {
            return Failure(
                AssemblyResourceResolutionStatus::InvalidCatalogIdentity,
                LookupFailureMessage(
                    "catalog returned a malformed identity for",
                    request,
                    {},
                    limits.maxCatalogErrorBytes));
        }

        const IdentityKey identityKey{
            lookup.identity.kind,
            lookup.identity.value,
            lookup.identity.generation
        };
        const auto [digestIt, inserted] = identityDigests.emplace(
            identityKey, lookup.identity.contentSha256);
        if (!inserted && digestIt->second != lookup.identity.contentSha256)
        {
            return Failure(
                AssemblyResourceResolutionStatus::IdentityConflict,
                LookupFailureMessage(
                    "catalog reused one identity for conflicting content at",
                    request,
                    {},
                    limits.maxCatalogErrorBytes));
        }
        accepted.push_back(AcceptedRequest{ request, std::move(lookup.identity) });
    }

    ResolvedAssemblyResources resolved;
    resolved.assembly = assembly;
    resolved.sourceManifestSha256 = assembly->sourceManifestSha256;
    resolved.modules.reserve(assembly->modules.size());
    resolved.zones.reserve(assembly->zones.size());
    resolved.movingParts.reserve(assembly->movingParts.size());

    for (const AssemblyModule& module : assembly->modules)
    {
        ResolvedAssemblyModuleResources moduleResources;
        const AssemblyCatalogIdentity* visual = FindAccepted(
            accepted, AssemblyResourceKind::Visual, module.visualSource);
        const AssemblyCatalogIdentity* collision = FindAccepted(
            accepted, AssemblyResourceKind::Collision, module.collisionSource);
        if (!visual || !collision)
        {
            return Failure(
                AssemblyResourceResolutionStatus::InternalError,
                "validated module resource request disappeared before publication");
        }
        moduleResources.visual =
            ToTyped<AssemblyResourceKind::Visual>(*visual);
        moduleResources.collision =
            ToTyped<AssemblyResourceKind::Collision>(*collision);
        moduleResources.lods.reserve(module.lods.size());
        for (const AssemblyLod& lod : module.lods)
        {
            const AssemblyCatalogIdentity* lodVisual = FindAccepted(
                accepted, AssemblyResourceKind::Visual, lod.source);
            if (!lodVisual)
            {
                return Failure(
                    AssemblyResourceResolutionStatus::InternalError,
                    "validated LOD resource request disappeared before publication");
            }
            moduleResources.lods.push_back(
                ToTyped<AssemblyResourceKind::Visual>(*lodVisual));
        }
        resolved.modules.push_back(std::move(moduleResources));
    }

    for (const AssemblyZone& zone : assembly->zones)
    {
        const AssemblyCatalogIdentity* navigationMesh = FindAccepted(
            accepted, AssemblyResourceKind::NavigationMesh, zone.navmeshSource);
        const AssemblyCatalogIdentity* walkableSurface = FindAccepted(
            accepted,
            AssemblyResourceKind::WalkableSurface,
            zone.walkableSurface);
        if (!navigationMesh || !walkableSurface)
        {
            return Failure(
                AssemblyResourceResolutionStatus::InternalError,
                "validated zone resource request disappeared before publication");
        }
        resolved.zones.push_back(ResolvedAssemblyZoneResources{
            ToTyped<AssemblyResourceKind::NavigationMesh>(*navigationMesh),
            ToTyped<AssemblyResourceKind::WalkableSurface>(*walkableSurface)
        });
    }

    for (const AssemblyMovingPart& part : assembly->movingParts)
    {
        const AssemblyCatalogIdentity* visual = FindAccepted(
            accepted, AssemblyResourceKind::Visual, part.visualSource);
        if (!visual)
        {
            return Failure(
                AssemblyResourceResolutionStatus::InternalError,
                "validated moving-part request disappeared before publication");
        }
        resolved.movingParts.push_back(ResolvedAssemblyMovingPartResources{
            ToTyped<AssemblyResourceKind::Visual>(*visual)
        });
    }

    AssemblyResourceResolutionResult result;
    result.status = AssemblyResourceResolutionStatus::Success;
    result.resources =
        std::make_shared<const ResolvedAssemblyResources>(std::move(resolved));
    return result;
}

} // namespace

const char* AssemblyResourceKindName(AssemblyResourceKind kind)
{
    switch (kind)
    {
    case AssemblyResourceKind::Visual: return "visual";
    case AssemblyResourceKind::Collision: return "collision";
    case AssemblyResourceKind::NavigationMesh: return "navigation mesh";
    case AssemblyResourceKind::WalkableSurface: return "walkable surface";
    }
    return "unknown";
}

bool AssemblyCatalogIdentity::IsValid() const
{
    return IsKnownKind(kind) && value != 0 && IsNonZero(contentSha256);
}

const char* AssemblyResourceResolutionStatusName(
    AssemblyResourceResolutionStatus status)
{
    switch (status)
    {
    case AssemblyResourceResolutionStatus::Success: return "success";
    case AssemblyResourceResolutionStatus::InvalidArgument: return "invalid argument";
    case AssemblyResourceResolutionStatus::InvalidAssembly: return "invalid assembly";
    case AssemblyResourceResolutionStatus::ResourceLimitExceeded: return "resource limit exceeded";
    case AssemblyResourceResolutionStatus::CatalogNotFound: return "catalog not found";
    case AssemblyResourceResolutionStatus::CatalogStale: return "catalog stale";
    case AssemblyResourceResolutionStatus::CatalogError: return "catalog error";
    case AssemblyResourceResolutionStatus::InvalidCatalogIdentity: return "invalid catalog identity";
    case AssemblyResourceResolutionStatus::CatalogKindMismatch: return "catalog kind mismatch";
    case AssemblyResourceResolutionStatus::IdentityConflict: return "identity conflict";
    case AssemblyResourceResolutionStatus::AllocationFailure: return "allocation failure";
    case AssemblyResourceResolutionStatus::InternalError: return "internal error";
    }
    return "unknown";
}

AssemblyResourceResolutionResult ResolveAssemblyResources(
    std::shared_ptr<const CookedAssembly> assembly,
    const AssemblyResourceCatalog& catalog,
    const AssemblyResourceResolutionLimits& limits)
{
    try
    {
        return ResolveImpl(assembly, catalog, limits);
    }
    catch (const std::bad_alloc&)
    {
        return Failure(
            AssemblyResourceResolutionStatus::AllocationFailure,
            "allocation failed during assembly resource resolution");
    }
    catch (const std::exception& exception)
    {
        constexpr size_t kMaxUnexpectedErrorBytes = 1024;
        const std::string_view detail(exception.what() ? exception.what() : "");
        return Failure(
            AssemblyResourceResolutionStatus::InternalError,
            std::string("unexpected resolver failure: ") +
                std::string(detail.substr(0, kMaxUnexpectedErrorBytes)));
    }
    catch (...)
    {
        return Failure(
            AssemblyResourceResolutionStatus::InternalError,
            "unexpected resolver failure");
    }
}

} // namespace asset
