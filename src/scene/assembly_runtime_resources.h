#pragma once

#include "assembly_instantiator.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace scene
{

enum class AssemblyRuntimeOwnerStatus : uint8_t
{
    Success,
    InvalidArgument,
    Duplicate,
    NotFound,
    Conflict,
    Sealed,
    AllocationFailure,
    InternalError
};

const char* AssemblyRuntimeOwnerStatusName(AssemblyRuntimeOwnerStatus status);

struct AssemblyRuntimeOwnerMutationResult
{
    AssemblyRuntimeOwnerStatus status =
        AssemblyRuntimeOwnerStatus::InvalidArgument;
    uint64_t ownerToken = 0;

    bool Succeeded() const
    {
        return status == AssemblyRuntimeOwnerStatus::Success;
    }
};

struct AssemblyRuntimeOwnerRegistration
{
    asset::AssemblyResourceKind kind = asset::AssemblyResourceKind::Visual;
    std::string locator;
    asset::Sha256Digest contentSha256;
    PreparedAssemblyVisual visual;
};

// Startup-only mutable table that becomes an immutable owner-system adapter
// before WS-025 preparation. Tokens are stable 1-based record indices and are
// meaningful only together with the exact catalog identity bound before Seal.
class AssemblyRuntimeResourceOwners final : public AssemblyRuntimeResourceAdapter
{
public:
    AssemblyRuntimeOwnerMutationResult Register(
        const AssemblyRuntimeOwnerRegistration& registration);
    AssemblyRuntimeOwnerStatus BindCatalogIdentity(
        uint64_t ownerToken,
        const asset::AssemblyCatalogIdentity& identity);
    AssemblyRuntimeOwnerStatus Seal();

    bool IsSealed() const { return m_sealed; }
    size_t RecordCount() const { return m_records.size(); }

    AssemblyRuntimeAdapterStatus PrepareVisual(
        const asset::AssemblyVisualIdentity& identity,
        uint64_t ownerToken,
        PreparedAssemblyVisual& visual) const override;
    AssemblyRuntimeAdapterStatus ValidateCollision(
        const asset::AssemblyCollisionIdentity& identity,
        uint64_t ownerToken) const override;
    AssemblyRuntimeAdapterStatus ValidateNavigationMesh(
        const asset::AssemblyNavigationMeshIdentity& identity,
        uint64_t ownerToken) const override;
    AssemblyRuntimeAdapterStatus ValidateWalkableSurface(
        const asset::AssemblyWalkableSurfaceIdentity& identity,
        uint64_t ownerToken) const override;

private:
    struct Record
    {
        asset::AssemblyResourceKind kind = asset::AssemblyResourceKind::Visual;
        std::string locator;
        asset::Sha256Digest contentSha256;
        asset::AssemblyCatalogIdentity identity;
        PreparedAssemblyVisual visual;
    };

    template <asset::AssemblyResourceKind Kind>
    AssemblyRuntimeAdapterStatus Validate(
        const asset::TypedAssemblyResourceIdentity<Kind>& identity,
        uint64_t ownerToken,
        const Record*& record) const;

    std::vector<Record> m_records;
    std::map<std::pair<uint8_t, std::string>, uint64_t> m_tokens;
    bool m_sealed = false;
};

} // namespace scene
