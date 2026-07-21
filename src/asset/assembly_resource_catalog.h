#pragma once

#include "assembly_resource_resolver.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace asset
{

struct AssemblyResourceRegistration
{
    AssemblyResourceKind kind = AssemblyResourceKind::Visual;
    std::string locator;
    Sha256Digest contentSha256;
    uint64_t ownerToken = 0;
    std::shared_ptr<const void> lifetimeAnchor;
};

struct AssemblyResourceCatalogRecord
{
    AssemblyCatalogIdentity identity;
    std::string locator;
    uint64_t ownerToken = 0;
    std::shared_ptr<const void> lifetimeAnchor;
    bool active = false;
};

enum class AssemblyResourceCatalogStatus : uint8_t
{
    Success,
    NoChange,
    InvalidArgument,
    Conflict,
    NotFound,
    Stale,
    ResourceLimitExceeded,
    GenerationExhausted,
    CatalogValueExhausted,
    EpochExhausted,
    AllocationFailure,
    InternalError
};

const char* AssemblyResourceCatalogStatusName(AssemblyResourceCatalogStatus status);

struct AssemblyResourceCatalogLimits
{
    uint64_t maxRecords = 500'000ull;
    uint64_t maxTotalLocatorBytes = 64ull * 1024ull * 1024ull;
    uint32_t maxLocatorBytes = 1024u * 1024u;
    uint32_t maxGeneration = UINT32_MAX;
    uint64_t maxCatalogValue = UINT64_MAX;
    uint64_t maxEpoch = UINT64_MAX;
};

struct AssemblyResourceCatalogMutationResult
{
    AssemblyResourceCatalogStatus status =
        AssemblyResourceCatalogStatus::InvalidArgument;
    AssemblyCatalogIdentity identity;
    std::string error;

    bool Succeeded() const
    {
        return status == AssemblyResourceCatalogStatus::Success ||
               status == AssemblyResourceCatalogStatus::NoChange;
    }
};

class AssemblyResourceCatalogSnapshot final : public AssemblyResourceCatalog
{
public:
    AssemblyCatalogLookup Resolve(
        AssemblyResourceKind kind,
        std::string_view locator) const override;

    bool TryGetOwnerToken(
        const AssemblyCatalogIdentity& identity,
        uint64_t& ownerToken) const;

    uint64_t Epoch() const;
    size_t RecordCount() const;
    size_t ActiveRecordCount() const;
    std::span<const AssemblyResourceCatalogRecord> Records() const;

private:
    struct Data;
    explicit AssemblyResourceCatalogSnapshot(std::shared_ptr<const Data> data);

    std::shared_ptr<const Data> m_data;
    friend class AssemblyResourceCatalogStore;
};

struct AssemblyResourceCatalogSnapshotResult
{
    AssemblyResourceCatalogStatus status =
        AssemblyResourceCatalogStatus::InvalidArgument;
    std::shared_ptr<const AssemblyResourceCatalogSnapshot> snapshot;
    std::string error;

    bool Succeeded() const
    {
        return status == AssemblyResourceCatalogStatus::Success &&
               snapshot != nullptr;
    }
};

class AssemblyResourceCatalogStore final : public AssemblyResourceCatalog
{
public:
    explicit AssemblyResourceCatalogStore(
        const AssemblyResourceCatalogLimits& limits = {});
    ~AssemblyResourceCatalogStore();

    AssemblyResourceCatalogStore(const AssemblyResourceCatalogStore&) = delete;
    AssemblyResourceCatalogStore& operator=(
        const AssemblyResourceCatalogStore&) = delete;

    AssemblyResourceCatalogMutationResult Register(
        const AssemblyResourceRegistration& registration);

    AssemblyResourceCatalogMutationResult Replace(
        const AssemblyCatalogIdentity& expectedIdentity,
        const AssemblyResourceRegistration& replacement);

    AssemblyResourceCatalogMutationResult Remove(
        AssemblyResourceKind kind,
        std::string_view locator,
        const AssemblyCatalogIdentity& expectedIdentity);

    AssemblyCatalogLookup Resolve(
        AssemblyResourceKind kind,
        std::string_view locator) const override;

    AssemblyResourceCatalogSnapshotResult AcquireSnapshot() const;

    uint64_t Epoch() const;
    size_t RecordCount() const;
    size_t ActiveRecordCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace asset
