#include "assembly_resource_catalog.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace asset
{
namespace
{

struct CatalogKey
{
    AssemblyResourceKind kind = AssemblyResourceKind::Visual;
    std::string locator;
};

struct CatalogKeyView
{
    AssemblyResourceKind kind = AssemblyResourceKind::Visual;
    std::string_view locator;
};

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

struct CatalogKeyLess
{
    using is_transparent = void;

    bool operator()(const CatalogKey& left, const CatalogKey& right) const
    {
        return Less({ left.kind, left.locator }, { right.kind, right.locator });
    }

    bool operator()(const CatalogKey& left, CatalogKeyView right) const
    {
        return Less({ left.kind, left.locator }, right);
    }

    bool operator()(CatalogKeyView left, const CatalogKey& right) const
    {
        return Less(left, { right.kind, right.locator });
    }

    static bool Less(CatalogKeyView left, CatalogKeyView right)
    {
        if (left.kind != right.kind)
        {
            return static_cast<uint8_t>(left.kind) <
                   static_cast<uint8_t>(right.kind);
        }
        return CompareLocator(left.locator, right.locator) < 0;
    }
};

struct IdentityKey
{
    AssemblyResourceKind kind = AssemblyResourceKind::Visual;
    uint64_t value = 0;
    uint32_t generation = 0;
    Sha256Digest contentSha256;

    bool operator<(const IdentityKey& other) const
    {
        if (kind != other.kind)
            return static_cast<uint8_t>(kind) < static_cast<uint8_t>(other.kind);
        if (value != other.value)
            return value < other.value;
        if (generation != other.generation)
            return generation < other.generation;
        return contentSha256.bytes < other.contentSha256.bytes;
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

bool ValidLimits(const AssemblyResourceCatalogLimits& limits)
{
    return limits.maxRecords > 0 && limits.maxTotalLocatorBytes > 0 &&
           limits.maxLocatorBytes > 0 && limits.maxGeneration > 0 &&
           limits.maxCatalogValue > 0 && limits.maxEpoch > 0;
}

bool ValidRegistration(
    const AssemblyResourceRegistration& registration,
    const AssemblyResourceCatalogLimits& limits)
{
    return IsKnownKind(registration.kind) && registration.ownerToken != 0 &&
           registration.lifetimeAnchor != nullptr &&
           IsNonZero(registration.contentSha256) &&
           IsSafeLocator(registration.locator, limits.maxLocatorBytes);
}

bool SameIdentity(
    const AssemblyCatalogIdentity& left,
    const AssemblyCatalogIdentity& right)
{
    return left == right;
}

bool SameLifetimeAnchor(
    const std::shared_ptr<const void>& left,
    const std::shared_ptr<const void>& right)
{
    return !left.owner_before(right) && !right.owner_before(left);
}

AssemblyResourceCatalogMutationResult MutationFailure(
    AssemblyResourceCatalogStatus status,
    std::string error)
{
    AssemblyResourceCatalogMutationResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

AssemblyResourceCatalogMutationResult MutationSuccess(
    AssemblyResourceCatalogStatus status,
    const AssemblyCatalogIdentity& identity = {})
{
    AssemblyResourceCatalogMutationResult result;
    result.status = status;
    result.identity = identity;
    return result;
}

AssemblyCatalogLookup LookupFailure(
    AssemblyCatalogLookupStatus status,
    std::string error)
{
    AssemblyCatalogLookup result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

AssemblyCatalogLookup LookupRecord(const AssemblyResourceCatalogRecord& record)
{
    if (!record.active)
        return LookupFailure(AssemblyCatalogLookupStatus::Stale, "resource was removed");
    AssemblyCatalogLookup result;
    result.status = AssemblyCatalogLookupStatus::Found;
    result.identity = record.identity;
    return result;
}

IdentityKey MakeIdentityKey(const AssemblyCatalogIdentity& identity)
{
    return IdentityKey{
        identity.kind,
        identity.value,
        identity.generation,
        identity.contentSha256
    };
}

} // namespace

struct AssemblyResourceCatalogSnapshot::Data
{
    uint64_t epoch = 0;
    size_t activeRecordCount = 0;
    uint32_t maxLocatorBytes = 0;
    std::vector<AssemblyResourceCatalogRecord> records;
    std::map<IdentityKey, uint64_t> ownerTokens;
};

struct AssemblyResourceCatalogStore::Impl
{
    explicit Impl(const AssemblyResourceCatalogLimits& requestedLimits)
        : limits(requestedLimits), limitsValid(ValidLimits(requestedLimits))
    {
    }

    AssemblyResourceCatalogLimits limits;
    bool limitsValid = false;
    mutable std::shared_mutex mutex;
    std::map<CatalogKey, AssemblyResourceCatalogRecord, CatalogKeyLess> records;
    uint64_t totalLocatorBytes = 0;
    uint64_t nextCatalogValue = 1;
    uint64_t epoch = 0;
    size_t activeRecordCount = 0;
    bool catalogValueExhausted = false;
};

const char* AssemblyResourceCatalogStatusName(AssemblyResourceCatalogStatus status)
{
    switch (status)
    {
    case AssemblyResourceCatalogStatus::Success: return "success";
    case AssemblyResourceCatalogStatus::NoChange: return "no change";
    case AssemblyResourceCatalogStatus::InvalidArgument: return "invalid argument";
    case AssemblyResourceCatalogStatus::Conflict: return "conflict";
    case AssemblyResourceCatalogStatus::NotFound: return "not found";
    case AssemblyResourceCatalogStatus::Stale: return "stale";
    case AssemblyResourceCatalogStatus::ResourceLimitExceeded: return "resource limit exceeded";
    case AssemblyResourceCatalogStatus::GenerationExhausted: return "generation exhausted";
    case AssemblyResourceCatalogStatus::CatalogValueExhausted: return "catalog value exhausted";
    case AssemblyResourceCatalogStatus::EpochExhausted: return "epoch exhausted";
    case AssemblyResourceCatalogStatus::AllocationFailure: return "allocation failure";
    case AssemblyResourceCatalogStatus::InternalError: return "internal error";
    }
    return "unknown";
}

AssemblyResourceCatalogSnapshot::AssemblyResourceCatalogSnapshot(
    std::shared_ptr<const Data> data)
    : m_data(std::move(data))
{
}

AssemblyCatalogLookup AssemblyResourceCatalogSnapshot::Resolve(
    AssemblyResourceKind kind,
    std::string_view locator) const
{
    if (!m_data || !IsKnownKind(kind) ||
        !IsSafeLocator(locator, m_data->maxLocatorBytes))
        return LookupFailure(AssemblyCatalogLookupStatus::Error, "invalid snapshot query");
    const CatalogKeyView key{ kind, locator };
    const auto it = std::lower_bound(
        m_data->records.begin(), m_data->records.end(), key,
        [](const AssemblyResourceCatalogRecord& record, CatalogKeyView value) {
            return CatalogKeyLess::Less(
                { record.identity.kind, record.locator }, value);
        });
    if (it == m_data->records.end() || it->identity.kind != kind ||
        it->locator != locator)
    {
        return LookupFailure(AssemblyCatalogLookupStatus::NotFound, "resource not found");
    }
    return LookupRecord(*it);
}

bool AssemblyResourceCatalogSnapshot::TryGetOwnerToken(
    const AssemblyCatalogIdentity& identity,
    uint64_t& ownerToken) const
{
    ownerToken = 0;
    if (!m_data || !identity.IsValid())
        return false;
    const auto it = m_data->ownerTokens.find(MakeIdentityKey(identity));
    if (it == m_data->ownerTokens.end())
        return false;
    ownerToken = it->second;
    return true;
}

uint64_t AssemblyResourceCatalogSnapshot::Epoch() const
{
    return m_data ? m_data->epoch : 0;
}

size_t AssemblyResourceCatalogSnapshot::RecordCount() const
{
    return m_data ? m_data->records.size() : 0;
}

size_t AssemblyResourceCatalogSnapshot::ActiveRecordCount() const
{
    return m_data ? m_data->activeRecordCount : 0;
}

std::span<const AssemblyResourceCatalogRecord>
AssemblyResourceCatalogSnapshot::Records() const
{
    if (!m_data)
        return {};
    return m_data->records;
}

AssemblyResourceCatalogStore::AssemblyResourceCatalogStore(
    const AssemblyResourceCatalogLimits& limits)
    : m_impl(std::make_unique<Impl>(limits))
{
}

AssemblyResourceCatalogStore::~AssemblyResourceCatalogStore() = default;

AssemblyResourceCatalogMutationResult AssemblyResourceCatalogStore::Register(
    const AssemblyResourceRegistration& registration)
{
    if (!m_impl || !m_impl->limitsValid ||
        !ValidRegistration(registration, m_impl->limits))
    {
        return MutationFailure(
            AssemblyResourceCatalogStatus::InvalidArgument,
            "registration or catalog limits are invalid");
    }

    try
    {
        std::unique_lock lock(m_impl->mutex);
        const CatalogKeyView key{ registration.kind, registration.locator };
        const auto existing = m_impl->records.find(key);
        if (existing != m_impl->records.end())
        {
            AssemblyResourceCatalogRecord& record = existing->second;
            if (record.active)
            {
                if (record.ownerToken == registration.ownerToken &&
                    record.identity.contentSha256 == registration.contentSha256 &&
                    SameLifetimeAnchor(
                        record.lifetimeAnchor, registration.lifetimeAnchor))
                {
                    return MutationSuccess(
                        AssemblyResourceCatalogStatus::NoChange,
                        record.identity);
                }
                return MutationFailure(
                    AssemblyResourceCatalogStatus::Conflict,
                    "active locator already has different content or owner token");
            }
            if (record.identity.generation >= m_impl->limits.maxGeneration)
            {
                return MutationFailure(
                    AssemblyResourceCatalogStatus::GenerationExhausted,
                    "catalog generation cannot advance");
            }
            if (m_impl->epoch >= m_impl->limits.maxEpoch)
            {
                return MutationFailure(
                    AssemblyResourceCatalogStatus::EpochExhausted,
                    "catalog epoch cannot advance");
            }
            ++record.identity.generation;
            record.identity.contentSha256 = registration.contentSha256;
            record.ownerToken = registration.ownerToken;
            record.lifetimeAnchor = registration.lifetimeAnchor;
            record.active = true;
            ++m_impl->activeRecordCount;
            ++m_impl->epoch;
            return MutationSuccess(
                AssemblyResourceCatalogStatus::Success,
                record.identity);
        }

        if (m_impl->records.size() >= m_impl->limits.maxRecords ||
            registration.locator.size() >
                m_impl->limits.maxTotalLocatorBytes - m_impl->totalLocatorBytes)
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::ResourceLimitExceeded,
                "catalog record or aggregate locator limit exceeded");
        }
        if (m_impl->catalogValueExhausted ||
            m_impl->nextCatalogValue > m_impl->limits.maxCatalogValue)
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::CatalogValueExhausted,
                "catalog value cannot advance");
        }
        if (m_impl->epoch >= m_impl->limits.maxEpoch)
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::EpochExhausted,
                "catalog epoch cannot advance");
        }

        AssemblyResourceCatalogRecord record;
        record.identity.kind = registration.kind;
        record.identity.value = m_impl->nextCatalogValue;
        record.identity.generation = 1;
        record.identity.contentSha256 = registration.contentSha256;
        record.locator = registration.locator;
        record.ownerToken = registration.ownerToken;
        record.lifetimeAnchor = registration.lifetimeAnchor;
        record.active = true;

        const AssemblyCatalogIdentity identity = record.identity;
        const auto [insertedRecord, inserted] = m_impl->records.emplace(
            CatalogKey{ registration.kind, registration.locator },
            std::move(record));
        if (!inserted)
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::InternalError,
                "catalog insertion lost uniqueness under exclusive lock");
        }
        (void)insertedRecord;
        m_impl->totalLocatorBytes += registration.locator.size();
        ++m_impl->activeRecordCount;
        ++m_impl->epoch;
        if (m_impl->nextCatalogValue == m_impl->limits.maxCatalogValue)
            m_impl->catalogValueExhausted = true;
        else
            ++m_impl->nextCatalogValue;
        return MutationSuccess(AssemblyResourceCatalogStatus::Success, identity);
    }
    catch (const std::bad_alloc&)
    {
        return MutationFailure(
            AssemblyResourceCatalogStatus::AllocationFailure,
            "allocation failed while registering catalog resource");
    }
    catch (...)
    {
        return MutationFailure(
            AssemblyResourceCatalogStatus::InternalError,
            "unexpected catalog registration failure");
    }
}

AssemblyResourceCatalogMutationResult AssemblyResourceCatalogStore::Replace(
    const AssemblyCatalogIdentity& expectedIdentity,
    const AssemblyResourceRegistration& replacement)
{
    if (!m_impl || !m_impl->limitsValid || !expectedIdentity.IsValid() ||
        !ValidRegistration(replacement, m_impl->limits) ||
        expectedIdentity.kind != replacement.kind)
    {
        return MutationFailure(
            AssemblyResourceCatalogStatus::InvalidArgument,
            "replacement, expected identity, or catalog limits are invalid");
    }

    try
    {
        std::unique_lock lock(m_impl->mutex);
        const auto it = m_impl->records.find(
            CatalogKeyView{ replacement.kind, replacement.locator });
        if (it == m_impl->records.end())
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::NotFound,
                "replacement locator was not registered");
        }
        AssemblyResourceCatalogRecord& record = it->second;
        if (!record.active || !SameIdentity(record.identity, expectedIdentity))
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::Stale,
                "replacement expected identity is stale");
        }
        if (record.ownerToken == replacement.ownerToken &&
            record.identity.contentSha256 == replacement.contentSha256 &&
            SameLifetimeAnchor(record.lifetimeAnchor, replacement.lifetimeAnchor))
        {
            return MutationSuccess(
                AssemblyResourceCatalogStatus::NoChange,
                record.identity);
        }
        if (record.identity.generation >= m_impl->limits.maxGeneration)
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::GenerationExhausted,
                "catalog generation cannot advance");
        }
        if (m_impl->epoch >= m_impl->limits.maxEpoch)
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::EpochExhausted,
                "catalog epoch cannot advance");
        }

        ++record.identity.generation;
        record.identity.contentSha256 = replacement.contentSha256;
        record.ownerToken = replacement.ownerToken;
        record.lifetimeAnchor = replacement.lifetimeAnchor;
        ++m_impl->epoch;
        return MutationSuccess(
            AssemblyResourceCatalogStatus::Success,
            record.identity);
    }
    catch (const std::bad_alloc&)
    {
        return MutationFailure(
            AssemblyResourceCatalogStatus::AllocationFailure,
            "allocation failed while replacing catalog resource");
    }
    catch (...)
    {
        return MutationFailure(
            AssemblyResourceCatalogStatus::InternalError,
            "unexpected catalog replacement failure");
    }
}

AssemblyResourceCatalogMutationResult AssemblyResourceCatalogStore::Remove(
    AssemblyResourceKind kind,
    std::string_view locator,
    const AssemblyCatalogIdentity& expectedIdentity)
{
    if (!m_impl || !m_impl->limitsValid || !expectedIdentity.IsValid() ||
        expectedIdentity.kind != kind ||
        !IsSafeLocator(locator, m_impl->limits.maxLocatorBytes))
    {
        return MutationFailure(
            AssemblyResourceCatalogStatus::InvalidArgument,
            "removal locator, expected identity, or catalog limits are invalid");
    }

    try
    {
        std::unique_lock lock(m_impl->mutex);
        const auto it = m_impl->records.find(CatalogKeyView{ kind, locator });
        if (it == m_impl->records.end())
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::NotFound,
                "removal locator was not registered");
        }
        AssemblyResourceCatalogRecord& record = it->second;
        if (!record.active || !SameIdentity(record.identity, expectedIdentity))
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::Stale,
                "removal expected identity is stale");
        }
        if (record.identity.generation >= m_impl->limits.maxGeneration)
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::GenerationExhausted,
                "catalog generation cannot advance");
        }
        if (m_impl->epoch >= m_impl->limits.maxEpoch)
        {
            return MutationFailure(
                AssemblyResourceCatalogStatus::EpochExhausted,
                "catalog epoch cannot advance");
        }

        ++record.identity.generation;
        record.identity.contentSha256 = {};
        record.ownerToken = 0;
        record.lifetimeAnchor.reset();
        record.active = false;
        --m_impl->activeRecordCount;
        ++m_impl->epoch;
        return MutationSuccess(AssemblyResourceCatalogStatus::Success);
    }
    catch (const std::bad_alloc&)
    {
        return MutationFailure(
            AssemblyResourceCatalogStatus::AllocationFailure,
            "allocation failed while removing catalog resource");
    }
    catch (...)
    {
        return MutationFailure(
            AssemblyResourceCatalogStatus::InternalError,
            "unexpected catalog removal failure");
    }
}

AssemblyCatalogLookup AssemblyResourceCatalogStore::Resolve(
    AssemblyResourceKind kind,
    std::string_view locator) const
{
    if (!m_impl || !m_impl->limitsValid || !IsKnownKind(kind) ||
        !IsSafeLocator(locator, m_impl->limits.maxLocatorBytes))
    {
        return LookupFailure(AssemblyCatalogLookupStatus::Error, "invalid catalog query");
    }

    try
    {
        std::shared_lock lock(m_impl->mutex);
        const auto it = m_impl->records.find(CatalogKeyView{ kind, locator });
        if (it == m_impl->records.end())
            return LookupFailure(AssemblyCatalogLookupStatus::NotFound, "resource not found");
        return LookupRecord(it->second);
    }
    catch (...)
    {
        return LookupFailure(AssemblyCatalogLookupStatus::Error, "catalog lookup failed");
    }
}

AssemblyResourceCatalogSnapshotResult
AssemblyResourceCatalogStore::AcquireSnapshot() const
{
    AssemblyResourceCatalogSnapshotResult result;
    if (!m_impl || !m_impl->limitsValid)
    {
        result.status = AssemblyResourceCatalogStatus::InvalidArgument;
        result.error = "catalog limits are invalid";
        return result;
    }

    try
    {
        std::shared_lock lock(m_impl->mutex);
        auto data = std::make_shared<AssemblyResourceCatalogSnapshot::Data>();
        data->epoch = m_impl->epoch;
        data->activeRecordCount = m_impl->activeRecordCount;
        data->maxLocatorBytes = m_impl->limits.maxLocatorBytes;
        data->records.reserve(m_impl->records.size());
        size_t observedActiveRecords = 0;
        for (const auto& [key, record] : m_impl->records)
        {
            if (key.kind != record.identity.kind ||
                key.locator != record.locator ||
                !IsSafeLocator(record.locator, data->maxLocatorBytes) ||
                record.identity.value == 0 ||
                record.identity.generation == 0)
            {
                result.status = AssemblyResourceCatalogStatus::InternalError;
                result.error = "catalog snapshot contained malformed record";
                return result;
            }
            if (record.active)
            {
                if (!record.identity.IsValid() ||
                    record.ownerToken == 0 ||
                    !record.lifetimeAnchor)
                {
                    result.status = AssemblyResourceCatalogStatus::InternalError;
                    result.error = "catalog snapshot contained malformed active record";
                    return result;
                }
                ++observedActiveRecords;
            }
            else if (IsNonZero(record.identity.contentSha256) ||
                     record.ownerToken != 0 ||
                     record.lifetimeAnchor)
            {
                result.status = AssemblyResourceCatalogStatus::InternalError;
                result.error = "catalog snapshot contained malformed tombstone";
                return result;
            }

            data->records.push_back(record);
            if (record.active)
            {
                const auto [it, inserted] = data->ownerTokens.emplace(
                    MakeIdentityKey(record.identity), record.ownerToken);
                if (!inserted || it->second == 0)
                {
                    result.status = AssemblyResourceCatalogStatus::InternalError;
                    result.error = "catalog snapshot contained conflicting identity";
                    return result;
                }
            }
        }
        if (observedActiveRecords != data->activeRecordCount)
        {
            result.status = AssemblyResourceCatalogStatus::InternalError;
            result.error = "catalog snapshot active-record count was inconsistent";
            return result;
        }

        result.snapshot = std::shared_ptr<const AssemblyResourceCatalogSnapshot>(
            new AssemblyResourceCatalogSnapshot(std::move(data)));
        result.status = AssemblyResourceCatalogStatus::Success;
        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.status = AssemblyResourceCatalogStatus::AllocationFailure;
        result.error = "allocation failed while acquiring catalog snapshot";
        return result;
    }
    catch (...)
    {
        result.status = AssemblyResourceCatalogStatus::InternalError;
        result.error = "unexpected catalog snapshot failure";
        return result;
    }
}

uint64_t AssemblyResourceCatalogStore::Epoch() const
{
    if (!m_impl)
        return 0;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->epoch;
}

size_t AssemblyResourceCatalogStore::RecordCount() const
{
    if (!m_impl)
        return 0;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->records.size();
}

size_t AssemblyResourceCatalogStore::ActiveRecordCount() const
{
    if (!m_impl)
        return 0;
    std::shared_lock lock(m_impl->mutex);
    return m_impl->activeRecordCount;
}

} // namespace asset
