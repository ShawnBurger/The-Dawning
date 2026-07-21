#include "assembly_runtime_resources.h"

#include <new>

namespace scene
{

namespace
{

bool IsKnownKind(asset::AssemblyResourceKind kind)
{
    switch (kind)
    {
    case asset::AssemblyResourceKind::Visual:
    case asset::AssemblyResourceKind::Collision:
    case asset::AssemblyResourceKind::NavigationMesh:
    case asset::AssemblyResourceKind::WalkableSurface:
        return true;
    default:
        return false;
    }
}

bool IsNonZero(const asset::Sha256Digest& digest)
{
    for (uint8_t byte : digest.bytes)
        if (byte != 0)
            return true;
    return false;
}

} // namespace

const char* AssemblyRuntimeOwnerStatusName(AssemblyRuntimeOwnerStatus status)
{
    switch (status)
    {
    case AssemblyRuntimeOwnerStatus::Success: return "success";
    case AssemblyRuntimeOwnerStatus::InvalidArgument: return "invalid argument";
    case AssemblyRuntimeOwnerStatus::Duplicate: return "duplicate";
    case AssemblyRuntimeOwnerStatus::NotFound: return "not found";
    case AssemblyRuntimeOwnerStatus::Conflict: return "conflict";
    case AssemblyRuntimeOwnerStatus::Sealed: return "sealed";
    case AssemblyRuntimeOwnerStatus::AllocationFailure: return "allocation failure";
    case AssemblyRuntimeOwnerStatus::InternalError: return "internal error";
    default: return "unknown";
    }
}

AssemblyRuntimeOwnerMutationResult AssemblyRuntimeResourceOwners::Register(
    const AssemblyRuntimeOwnerRegistration& registration)
{
    AssemblyRuntimeOwnerMutationResult result;
    if (m_sealed)
    {
        result.status = AssemblyRuntimeOwnerStatus::Sealed;
        return result;
    }
    if (!IsKnownKind(registration.kind) || registration.locator.empty() ||
        !IsNonZero(registration.contentSha256) ||
        (registration.kind == asset::AssemblyResourceKind::Visual &&
         !registration.visual.mesh.IsValid()) ||
        (registration.kind != asset::AssemblyResourceKind::Visual &&
         registration.visual.mesh.IsValid()))
    {
        return result;
    }

    try
    {
        const auto key = std::make_pair(
            static_cast<uint8_t>(registration.kind), registration.locator);
        if (m_tokens.find(key) != m_tokens.end())
        {
            result.status = AssemblyRuntimeOwnerStatus::Duplicate;
            return result;
        }
        if (m_records.size() == UINT64_MAX)
        {
            result.status = AssemblyRuntimeOwnerStatus::InternalError;
            return result;
        }

        Record record;
        record.kind = registration.kind;
        record.locator = registration.locator;
        record.contentSha256 = registration.contentSha256;
        record.visual = registration.visual;
        m_records.push_back(std::move(record));
        const uint64_t token = static_cast<uint64_t>(m_records.size());
        try
        {
            m_tokens.emplace(key, token);
        }
        catch (...)
        {
            m_records.pop_back();
            throw;
        }

        result.status = AssemblyRuntimeOwnerStatus::Success;
        result.ownerToken = token;
        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.status = AssemblyRuntimeOwnerStatus::AllocationFailure;
        return result;
    }
    catch (...)
    {
        result.status = AssemblyRuntimeOwnerStatus::InternalError;
        return result;
    }
}

AssemblyRuntimeOwnerStatus AssemblyRuntimeResourceOwners::BindCatalogIdentity(
    uint64_t ownerToken,
    const asset::AssemblyCatalogIdentity& identity)
{
    if (m_sealed)
        return AssemblyRuntimeOwnerStatus::Sealed;
    if (ownerToken == 0 || ownerToken > m_records.size())
        return AssemblyRuntimeOwnerStatus::NotFound;
    Record& record = m_records[static_cast<size_t>(ownerToken - 1)];
    if (!identity.IsValid() || identity.generation == 0 ||
        identity.kind != record.kind ||
        identity.contentSha256 != record.contentSha256)
    {
        return AssemblyRuntimeOwnerStatus::Conflict;
    }
    if (record.identity.IsValid())
        return record.identity == identity
            ? AssemblyRuntimeOwnerStatus::Success
            : AssemblyRuntimeOwnerStatus::Conflict;
    record.identity = identity;
    return AssemblyRuntimeOwnerStatus::Success;
}

AssemblyRuntimeOwnerStatus AssemblyRuntimeResourceOwners::Seal()
{
    if (m_sealed)
        return AssemblyRuntimeOwnerStatus::Success;
    if (m_records.empty())
        return AssemblyRuntimeOwnerStatus::InvalidArgument;
    for (const Record& record : m_records)
        if (!record.identity.IsValid() || record.identity.generation == 0)
            return AssemblyRuntimeOwnerStatus::Conflict;
    m_sealed = true;
    return AssemblyRuntimeOwnerStatus::Success;
}

template <asset::AssemblyResourceKind Kind>
AssemblyRuntimeAdapterStatus AssemblyRuntimeResourceOwners::Validate(
    const asset::TypedAssemblyResourceIdentity<Kind>& identity,
    uint64_t ownerToken,
    const Record*& record) const
{
    record = nullptr;
    if (!m_sealed)
        return AssemblyRuntimeAdapterStatus::InternalError;
    if (ownerToken == 0 || ownerToken > m_records.size())
        return AssemblyRuntimeAdapterStatus::NotFound;

    const Record& candidate = m_records[static_cast<size_t>(ownerToken - 1)];
    if (candidate.kind != Kind)
        return AssemblyRuntimeAdapterStatus::Unsupported;
    if (!identity.IsValid() || identity.generation == 0 ||
        candidate.identity.value != identity.value ||
        candidate.identity.generation != identity.generation ||
        candidate.identity.contentSha256 != identity.contentSha256)
    {
        return AssemblyRuntimeAdapterStatus::Stale;
    }
    record = &candidate;
    return AssemblyRuntimeAdapterStatus::Success;
}

AssemblyRuntimeAdapterStatus AssemblyRuntimeResourceOwners::PrepareVisual(
    const asset::AssemblyVisualIdentity& identity,
    uint64_t ownerToken,
    PreparedAssemblyVisual& visual) const
{
    visual = {};
    const Record* record = nullptr;
    const AssemblyRuntimeAdapterStatus status =
        Validate(identity, ownerToken, record);
    if (status != AssemblyRuntimeAdapterStatus::Success)
        return status;
    if (!record->visual.mesh.IsValid())
        return AssemblyRuntimeAdapterStatus::InvalidResource;
    visual = record->visual;
    return AssemblyRuntimeAdapterStatus::Success;
}

AssemblyRuntimeAdapterStatus AssemblyRuntimeResourceOwners::ValidateCollision(
    const asset::AssemblyCollisionIdentity& identity,
    uint64_t ownerToken) const
{
    const Record* record = nullptr;
    return Validate(identity, ownerToken, record);
}

AssemblyRuntimeAdapterStatus AssemblyRuntimeResourceOwners::ValidateNavigationMesh(
    const asset::AssemblyNavigationMeshIdentity& identity,
    uint64_t ownerToken) const
{
    const Record* record = nullptr;
    return Validate(identity, ownerToken, record);
}

AssemblyRuntimeAdapterStatus AssemblyRuntimeResourceOwners::ValidateWalkableSurface(
    const asset::AssemblyWalkableSurfaceIdentity& identity,
    uint64_t ownerToken) const
{
    const Record* record = nullptr;
    return Validate(identity, ownerToken, record);
}

} // namespace scene
