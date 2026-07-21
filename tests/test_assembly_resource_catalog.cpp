#include "test_framework.h"
#include "asset/assembly_resource_catalog.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

asset::Sha256Digest HashText(std::string_view text)
{
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    return asset::ComputeSha256(std::span<const std::byte>(bytes, text.size()));
}

asset::AssemblyResourceRegistration Registration(
    asset::AssemblyResourceKind kind,
    std::string locator,
    std::string_view content,
    uint64_t ownerToken)
{
    asset::AssemblyResourceRegistration registration;
    registration.kind = kind;
    registration.locator = std::move(locator);
    registration.contentSha256 = HashText(content);
    registration.ownerToken = ownerToken;
    registration.lifetimeAnchor = std::make_shared<uint64_t>(ownerToken);
    return registration;
}

asset::AssemblyCatalogIdentity GenericIdentity(
    const asset::AssemblyVisualIdentity& identity)
{
    asset::AssemblyCatalogIdentity result;
    result.kind = asset::AssemblyResourceKind::Visual;
    result.value = identity.value;
    result.generation = identity.generation;
    result.contentSha256 = identity.contentSha256;
    return result;
}

void CheckMutationFailure(
    const asset::AssemblyResourceCatalogMutationResult& result,
    asset::AssemblyResourceCatalogStatus expected)
{
    CHECK_FALSE(result.Succeeded());
    CHECK_EQ(result.status, expected);
}

} // namespace

TEST_CASE(AssemblyResourceCatalog_RegistersIdempotentlyAndResolvesThroughSnapshot)
{
    asset::AssemblyResourceCatalogStore store;
    const auto visual = Registration(
        asset::AssemblyResourceKind::Visual,
        "visual://ship/hull",
        "hull bytes",
        101);

    const auto first = store.Register(visual);
    CHECK(first.Succeeded());
    CHECK_EQ(first.status, asset::AssemblyResourceCatalogStatus::Success);
    CHECK_EQ(first.identity.value, (uint64_t)1);
    CHECK_EQ(first.identity.generation, (uint32_t)1);
    CHECK_EQ(store.Epoch(), (uint64_t)1);
    CHECK_EQ(store.RecordCount(), (size_t)1);
    CHECK_EQ(store.ActiveRecordCount(), (size_t)1);

    const auto duplicate = store.Register(visual);
    CHECK(duplicate.Succeeded());
    CHECK_EQ(duplicate.status, asset::AssemblyResourceCatalogStatus::NoChange);
    CHECK_EQ(duplicate.identity, first.identity);
    CHECK_EQ(store.Epoch(), (uint64_t)1);

    const asset::AssemblyCatalogLookup live = store.Resolve(
        asset::AssemblyResourceKind::Visual, "visual://ship/hull");
    CHECK(live.Found());
    CHECK_EQ(live.identity, first.identity);

    const auto acquired = store.AcquireSnapshot();
    CHECK(acquired.Succeeded());
    CHECK_EQ(acquired.snapshot->Epoch(), (uint64_t)1);
    CHECK_EQ(acquired.snapshot->RecordCount(), (size_t)1);
    CHECK_EQ(acquired.snapshot->ActiveRecordCount(), (size_t)1);
    uint64_t ownerToken = 0;
    CHECK(acquired.snapshot->TryGetOwnerToken(first.identity, ownerToken));
    CHECK_EQ(ownerToken, (uint64_t)101);
    CHECK(acquired.snapshot->Resolve(
        asset::AssemblyResourceKind::Visual, "visual://ship/hull").Found());
    CHECK_EQ(
        acquired.snapshot->Resolve(
            asset::AssemblyResourceKind::Visual,
            "visual://bad\nquery").status,
        asset::AssemblyCatalogLookupStatus::Error);
}

TEST_CASE(AssemblyResourceCatalog_ConcreteSnapshotDrivesShippedAssemblyResolver)
{
    asset::AssemblyResourceCatalogStore store;
    const auto visual = store.Register(Registration(
        asset::AssemblyResourceKind::Visual,
        "visual://ship/hull",
        "hull visual",
        501));
    const auto collision = store.Register(Registration(
        asset::AssemblyResourceKind::Collision,
        "collision://ship/hull",
        "hull collision",
        601));
    CHECK(visual.Succeeded());
    CHECK(collision.Succeeded());

    const auto acquired = store.AcquireSnapshot();
    CHECK(acquired.Succeeded());

    auto assembly = std::make_shared<asset::CookedAssembly>();
    assembly->sourceManifestSha256 = HashText("manifest");
    asset::AssemblyModule module;
    module.id = "hull";
    module.visualSource = "visual://ship/hull";
    module.collisionSource = "collision://ship/hull";
    assembly->modules.push_back(module);

    const auto resolved = asset::ResolveAssemblyResources(
        assembly, *acquired.snapshot);
    CHECK(resolved.Succeeded());
    CHECK_EQ(resolved.resources->modules.size(), (size_t)1);
    CHECK_EQ(resolved.resources->modules[0].visual.value, visual.identity.value);
    CHECK_EQ(
        resolved.resources->modules[0].collision.value,
        collision.identity.value);

    uint64_t ownerToken = 0;
    CHECK(acquired.snapshot->TryGetOwnerToken(
        GenericIdentity(resolved.resources->modules[0].visual), ownerToken));
    CHECK_EQ(ownerToken, (uint64_t)501);
}

TEST_CASE(AssemblyResourceCatalog_SnapshotsRemainStableAcrossReplaceRemoveAndRevive)
{
    asset::AssemblyResourceCatalogStore store;
    const auto originalRegistration = Registration(
        asset::AssemblyResourceKind::Visual,
        "visual://door",
        "door v1",
        11);
    const auto original = store.Register(originalRegistration);
    CHECK(original.Succeeded());
    const auto originalSnapshot = store.AcquireSnapshot();
    CHECK(originalSnapshot.Succeeded());

    const auto replacementRegistration = Registration(
        asset::AssemblyResourceKind::Visual,
        "visual://door",
        "door v2",
        12);
    const auto replacement = store.Replace(
        original.identity, replacementRegistration);
    CHECK(replacement.Succeeded());
    CHECK_EQ(replacement.identity.value, original.identity.value);
    CHECK_EQ(replacement.identity.generation, (uint32_t)2);
    CHECK_EQ(store.Epoch(), (uint64_t)2);

    const auto replacementSnapshot = store.AcquireSnapshot();
    CHECK(replacementSnapshot.Succeeded());
    uint64_t ownerToken = 0;
    CHECK(originalSnapshot.snapshot->TryGetOwnerToken(
        original.identity, ownerToken));
    CHECK_EQ(ownerToken, (uint64_t)11);
    CHECK_FALSE(replacementSnapshot.snapshot->TryGetOwnerToken(
        original.identity, ownerToken));
    CHECK(replacementSnapshot.snapshot->TryGetOwnerToken(
        replacement.identity, ownerToken));
    CHECK_EQ(ownerToken, (uint64_t)12);

    const auto removed = store.Remove(
        asset::AssemblyResourceKind::Visual,
        "visual://door",
        replacement.identity);
    CHECK(removed.Succeeded());
    CHECK_EQ(store.Epoch(), (uint64_t)3);
    CHECK_EQ(store.ActiveRecordCount(), (size_t)0);
    CHECK_EQ(
        store.Resolve(asset::AssemblyResourceKind::Visual, "visual://door").status,
        asset::AssemblyCatalogLookupStatus::Stale);
    const auto removedSnapshot = store.AcquireSnapshot();
    CHECK(removedSnapshot.Succeeded());
    CHECK_EQ(removedSnapshot.snapshot->RecordCount(), (size_t)1);
    CHECK_EQ(removedSnapshot.snapshot->ActiveRecordCount(), (size_t)0);
    CHECK_FALSE(removedSnapshot.snapshot->TryGetOwnerToken(
        replacement.identity, ownerToken));
    CHECK(replacementSnapshot.snapshot->TryGetOwnerToken(
        replacement.identity, ownerToken));

    const auto revived = store.Register(Registration(
        asset::AssemblyResourceKind::Visual,
        "visual://door",
        "door v3",
        13));
    CHECK(revived.Succeeded());
    CHECK_EQ(revived.identity.value, original.identity.value);
    CHECK_EQ(revived.identity.generation, (uint32_t)4);
    CHECK_EQ(store.RecordCount(), (size_t)1);
    CHECK_EQ(store.ActiveRecordCount(), (size_t)1);
}

TEST_CASE(AssemblyResourceCatalog_SnapshotLeaseOutlivesStoreAndPinsOwnerAnchor)
{
    std::shared_ptr<const asset::AssemblyResourceCatalogSnapshot> lease;
    std::weak_ptr<const void> ownerLifetime;
    asset::AssemblyCatalogIdentity identity;
    {
        asset::AssemblyResourceCatalogStore store;
        auto registration = Registration(
            asset::AssemblyResourceKind::NavigationMesh,
            "nav://station/deck",
            "navigation bytes",
            901);
        ownerLifetime = registration.lifetimeAnchor;
        const auto registered = store.Register(registration);
        CHECK(registered.Succeeded());
        identity = registered.identity;
        const auto acquired = store.AcquireSnapshot();
        CHECK(acquired.Succeeded());
        lease = acquired.snapshot;
        registration.lifetimeAnchor.reset();
        CHECK_FALSE(ownerLifetime.expired());
    }

    CHECK_FALSE(ownerLifetime.expired());
    uint64_t ownerToken = 0;
    CHECK(lease->TryGetOwnerToken(identity, ownerToken));
    CHECK_EQ(ownerToken, (uint64_t)901);
    lease.reset();
    CHECK(ownerLifetime.expired());
}

TEST_CASE(AssemblyResourceCatalog_RejectsConflictsAndStaleCompareExchange)
{
    asset::AssemblyResourceCatalogStore store;
    const auto firstRegistration = Registration(
        asset::AssemblyResourceKind::Collision,
        "collision://hull",
        "collision v1",
        21);
    const auto first = store.Register(firstRegistration);
    CHECK(first.Succeeded());

    CheckMutationFailure(
        store.Register(Registration(
            asset::AssemblyResourceKind::Collision,
            "collision://hull",
            "collision conflict",
            22)),
        asset::AssemblyResourceCatalogStatus::Conflict);
    CHECK_EQ(store.Epoch(), (uint64_t)1);

    CheckMutationFailure(
        store.Register(Registration(
            asset::AssemblyResourceKind::Collision,
            "collision://hull",
            "collision v1",
            21)),
        asset::AssemblyResourceCatalogStatus::Conflict);
    CHECK_EQ(store.Epoch(), (uint64_t)1);

    asset::AssemblyCatalogIdentity wrongDigest = first.identity;
    wrongDigest.contentSha256 = HashText("wrong expected bytes");
    CheckMutationFailure(
        store.Replace(wrongDigest, Registration(
            asset::AssemblyResourceKind::Collision,
            "collision://hull",
            "collision v2",
            22)),
        asset::AssemblyResourceCatalogStatus::Stale);
    CheckMutationFailure(
        store.Remove(
            asset::AssemblyResourceKind::Collision,
            "collision://hull",
            wrongDigest),
        asset::AssemblyResourceCatalogStatus::Stale);

    const auto missingRegistration = Registration(
        asset::AssemblyResourceKind::Collision,
        "collision://missing",
        "missing",
        23);
    CheckMutationFailure(
        store.Replace(first.identity, missingRegistration),
        asset::AssemblyResourceCatalogStatus::NotFound);
    CheckMutationFailure(
        store.Remove(
            asset::AssemblyResourceKind::Collision,
            "collision://missing",
            first.identity),
        asset::AssemblyResourceCatalogStatus::NotFound);
    CHECK_EQ(store.Epoch(), (uint64_t)1);
}

TEST_CASE(AssemblyResourceCatalog_SnapshotOrderIsCanonicalNotInsertionOrder)
{
    asset::AssemblyResourceCatalogStore store;
    const auto collision = store.Register(Registration(
        asset::AssemblyResourceKind::Collision,
        "collision://z",
        "c",
        31));
    const auto visualB = store.Register(Registration(
        asset::AssemblyResourceKind::Visual,
        "visual://b",
        "b",
        32));
    const auto visualA = store.Register(Registration(
        asset::AssemblyResourceKind::Visual,
        "visual://a",
        "a",
        33));
    CHECK(collision.Succeeded());
    CHECK(visualB.Succeeded());
    CHECK(visualA.Succeeded());

    const auto snapshot = store.AcquireSnapshot();
    CHECK(snapshot.Succeeded());
    const auto records = snapshot.snapshot->Records();
    CHECK_EQ(records.size(), (size_t)3);
    CHECK_EQ(records[0].locator, "visual://a");
    CHECK_EQ(records[0].identity.value, visualA.identity.value);
    CHECK_EQ(records[1].locator, "visual://b");
    CHECK_EQ(records[1].identity.value, visualB.identity.value);
    CHECK_EQ(records[2].locator, "collision://z");
    CHECK_EQ(records[2].identity.value, collision.identity.value);
}

TEST_CASE(AssemblyResourceCatalog_ValidatesInputAndFailsClosedAtEveryLimit)
{
    asset::AssemblyResourceCatalogStore store;

    auto invalid = Registration(
        asset::AssemblyResourceKind::Visual, "visual://valid", "bytes", 1);
    invalid.kind = static_cast<asset::AssemblyResourceKind>(127);
    CheckMutationFailure(
        store.Register(invalid),
        asset::AssemblyResourceCatalogStatus::InvalidArgument);

    invalid = Registration(
        asset::AssemblyResourceKind::Visual, "", "bytes", 1);
    CheckMutationFailure(
        store.Register(invalid),
        asset::AssemblyResourceCatalogStatus::InvalidArgument);

    invalid = Registration(
        asset::AssemblyResourceKind::Visual, "visual://bad\nlocator", "bytes", 1);
    CheckMutationFailure(
        store.Register(invalid),
        asset::AssemblyResourceCatalogStatus::InvalidArgument);

    invalid = Registration(
        asset::AssemblyResourceKind::Visual, "visual://valid", "bytes", 0);
    CheckMutationFailure(
        store.Register(invalid),
        asset::AssemblyResourceCatalogStatus::InvalidArgument);

    invalid = Registration(
        asset::AssemblyResourceKind::Visual, "visual://valid", "bytes", 1);
    invalid.lifetimeAnchor.reset();
    CheckMutationFailure(
        store.Register(invalid),
        asset::AssemblyResourceCatalogStatus::InvalidArgument);

    invalid = Registration(
        asset::AssemblyResourceKind::Visual, "visual://valid", "bytes", 1);
    invalid.contentSha256 = {};
    CheckMutationFailure(
        store.Register(invalid),
        asset::AssemblyResourceCatalogStatus::InvalidArgument);

    const unsigned char invalidByte = 0xffu;
    invalid = Registration(
        asset::AssemblyResourceKind::Visual, "visual://", "bytes", 1);
    invalid.locator.push_back(static_cast<char>(invalidByte));
    CheckMutationFailure(
        store.Register(invalid),
        asset::AssemblyResourceCatalogStatus::InvalidArgument);
    CHECK_EQ(store.RecordCount(), (size_t)0);

    asset::AssemblyResourceCatalogLimits limits;
    limits.maxRecords = 1;
    asset::AssemblyResourceCatalogStore recordLimited(limits);
    CHECK(recordLimited.Register(Registration(
        asset::AssemblyResourceKind::Visual, "visual://one", "1", 1)).Succeeded());
    CheckMutationFailure(
        recordLimited.Register(Registration(
            asset::AssemblyResourceKind::Visual, "visual://two", "2", 2)),
        asset::AssemblyResourceCatalogStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxTotalLocatorBytes = 12;
    asset::AssemblyResourceCatalogStore bytesLimited(limits);
    CHECK(bytesLimited.Register(Registration(
        asset::AssemblyResourceKind::Visual, "visual://one", "1", 1)).Succeeded());
    CheckMutationFailure(
        bytesLimited.Register(Registration(
            asset::AssemblyResourceKind::Visual, "v", "2", 2)),
        asset::AssemblyResourceCatalogStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxCatalogValue = 1;
    asset::AssemblyResourceCatalogStore valueLimited(limits);
    CHECK(valueLimited.Register(Registration(
        asset::AssemblyResourceKind::Visual, "visual://one", "1", 1)).Succeeded());
    CheckMutationFailure(
        valueLimited.Register(Registration(
            asset::AssemblyResourceKind::Visual, "visual://two", "2", 2)),
        asset::AssemblyResourceCatalogStatus::CatalogValueExhausted);

    limits = {};
    limits.maxGeneration = 2;
    asset::AssemblyResourceCatalogStore generationLimited(limits);
    const auto generated = generationLimited.Register(Registration(
        asset::AssemblyResourceKind::Visual, "visual://one", "1", 1));
    CHECK(generated.Succeeded());
    CHECK(generationLimited.Remove(
        asset::AssemblyResourceKind::Visual,
        "visual://one",
        generated.identity).Succeeded());
    CheckMutationFailure(
        generationLimited.Register(Registration(
            asset::AssemblyResourceKind::Visual, "visual://one", "2", 2)),
        asset::AssemblyResourceCatalogStatus::GenerationExhausted);

    limits = {};
    limits.maxEpoch = 1;
    asset::AssemblyResourceCatalogStore epochLimited(limits);
    const auto epochRecord = epochLimited.Register(Registration(
        asset::AssemblyResourceKind::Visual, "visual://one", "1", 1));
    CHECK(epochRecord.Succeeded());
    CheckMutationFailure(
        epochLimited.Replace(epochRecord.identity, Registration(
            asset::AssemblyResourceKind::Visual, "visual://one", "2", 2)),
        asset::AssemblyResourceCatalogStatus::EpochExhausted);

    limits = {};
    limits.maxRecords = 0;
    asset::AssemblyResourceCatalogStore invalidStore(limits);
    CheckMutationFailure(
        invalidStore.Register(Registration(
            asset::AssemblyResourceKind::Visual, "visual://one", "1", 1)),
        asset::AssemblyResourceCatalogStatus::InvalidArgument);
    CHECK_FALSE(invalidStore.AcquireSnapshot().Succeeded());
    CHECK_EQ(
        invalidStore.Resolve(
            asset::AssemblyResourceKind::Visual, "visual://one").status,
        asset::AssemblyCatalogLookupStatus::Error);
}

TEST_CASE(AssemblyResourceCatalog_ConcurrentSnapshotsObserveWholeGenerations)
{
    asset::AssemblyResourceCatalogStore store;
    auto current = store.Register(Registration(
        asset::AssemblyResourceKind::Visual,
        "visual://streamed",
        "generation 0",
        1));
    CHECK(current.Succeeded());

    constexpr int kReaderCount = 4;
    constexpr int kReplacementCount = 200;
    std::atomic<int> readyReaders = 0;
    std::atomic<int> observations = 0;
    std::atomic<bool> start = false;
    std::atomic<bool> writerDone = false;
    std::atomic<bool> okay = true;
    std::vector<std::thread> readers;
    readers.reserve(kReaderCount);
    for (int reader = 0; reader < kReaderCount; ++reader)
    {
        readers.emplace_back([&]() {
            ++readyReaders;
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            uint64_t previousEpoch = 0;
            while (!writerDone.load(std::memory_order_acquire))
            {
                const auto snapshot = store.AcquireSnapshot();
                if (!snapshot.Succeeded() || snapshot.snapshot->Epoch() < previousEpoch)
                {
                    okay = false;
                    return;
                }
                previousEpoch = snapshot.snapshot->Epoch();
                ++observations;
                const auto lookup = snapshot.snapshot->Resolve(
                    asset::AssemblyResourceKind::Visual,
                    "visual://streamed");
                uint64_t ownerToken = 0;
                if (!lookup.Found() ||
                    !snapshot.snapshot->TryGetOwnerToken(
                        lookup.identity, ownerToken) || ownerToken == 0)
                {
                    okay = false;
                    return;
                }
                if (!store.Resolve(
                        asset::AssemblyResourceKind::Visual,
                        "visual://streamed").Found())
                {
                    okay = false;
                    return;
                }
            }
        });
    }

    while (readyReaders.load(std::memory_order_acquire) != kReaderCount)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    while (observations.load(std::memory_order_acquire) < kReaderCount)
        std::this_thread::yield();
    for (int generation = 1; generation <= kReplacementCount; ++generation)
    {
        const std::string content = "generation " + std::to_string(generation);
        const auto replaced = store.Replace(
            current.identity,
            Registration(
                asset::AssemblyResourceKind::Visual,
                "visual://streamed",
                content,
                static_cast<uint64_t>(generation + 1)));
        if (!replaced.Succeeded())
        {
            okay = false;
            break;
        }
        current = replaced;
    }
    writerDone.store(true, std::memory_order_release);
    for (std::thread& reader : readers)
        reader.join();

    CHECK(okay.load());
    CHECK(observations.load() >= kReaderCount);
    CHECK_EQ(current.identity.generation, (uint32_t)(kReplacementCount + 1));
    CHECK_EQ(store.Epoch(), (uint64_t)(kReplacementCount + 1));
    const auto finalSnapshot = store.AcquireSnapshot();
    CHECK(finalSnapshot.Succeeded());
    uint64_t ownerToken = 0;
    CHECK(finalSnapshot.snapshot->TryGetOwnerToken(
        current.identity, ownerToken));
    CHECK_EQ(ownerToken, (uint64_t)(kReplacementCount + 1));
}

TEST_CASE(AssemblyResourceCatalog_StatusNamesAndExactIdentityChecksAreStable)
{
    CHECK_EQ(
        std::string(asset::AssemblyResourceCatalogStatusName(
            asset::AssemblyResourceCatalogStatus::GenerationExhausted)),
        "generation exhausted");
    CHECK_EQ(
        std::string(asset::AssemblyResourceCatalogStatusName(
            static_cast<asset::AssemblyResourceCatalogStatus>(127))),
        "unknown");

    asset::AssemblyResourceCatalogStore store;
    const auto registered = store.Register(Registration(
        asset::AssemblyResourceKind::WalkableSurface,
        "walk://deck",
        "deck",
        77));
    CHECK(registered.Succeeded());
    const auto snapshot = store.AcquireSnapshot();
    CHECK(snapshot.Succeeded());
    uint64_t ownerToken = 0;
    CHECK(snapshot.snapshot->TryGetOwnerToken(
        registered.identity, ownerToken));
    asset::AssemblyCatalogIdentity wrongKind = registered.identity;
    wrongKind.kind = asset::AssemblyResourceKind::NavigationMesh;
    ownerToken = 999;
    CHECK_FALSE(snapshot.snapshot->TryGetOwnerToken(wrongKind, ownerToken));
    CHECK_EQ(ownerToken, (uint64_t)0);
    asset::AssemblyCatalogIdentity wrongGeneration = registered.identity;
    ++wrongGeneration.generation;
    ownerToken = 999;
    CHECK_FALSE(snapshot.snapshot->TryGetOwnerToken(
        wrongGeneration, ownerToken));
    CHECK_EQ(ownerToken, (uint64_t)0);
    asset::AssemblyCatalogIdentity wrongContent = registered.identity;
    wrongContent.contentSha256 = HashText("other deck");
    ownerToken = 999;
    CHECK_FALSE(snapshot.snapshot->TryGetOwnerToken(wrongContent, ownerToken));
    CHECK_EQ(ownerToken, (uint64_t)0);
}
