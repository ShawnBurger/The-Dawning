#include "source_snapshot.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <new>

namespace asset
{
namespace
{

SourceSnapshotStatus ReadBoundedFile(
    const std::filesystem::path& path,
    uint64_t maxBytes,
    std::vector<std::byte>& bytes)
{
    std::error_code errorCode;
    const uint64_t size = std::filesystem::file_size(path, errorCode);
    if (errorCode)
        return SourceSnapshotStatus::IoError;
    if (size > maxBytes || size > (std::numeric_limits<size_t>::max)() ||
        size > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)()))
    {
        return SourceSnapshotStatus::ResourceLimitExceeded;
    }
    try
    {
        bytes.resize(static_cast<size_t>(size));
    }
    catch (const std::bad_alloc&)
    {
        return SourceSnapshotStatus::ResourceLimitExceeded;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream || !stream.read(reinterpret_cast<char*>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size())))
    {
        return SourceSnapshotStatus::IoError;
    }
    return SourceSnapshotStatus::Success;
}

} // namespace

SourceSnapshotResult CaptureSourceDependencies(
    const std::filesystem::path& sourceDirectory,
    const std::vector<GltfSourceDependency>& dependencies,
    const GltfImportLimits& importLimits,
    const CookedModelLimits& cookedLimits)
{
    SourceSnapshotResult result;
    if (dependencies.size() > cookedLimits.maxDependencies)
    {
        result.status = SourceSnapshotStatus::ResourceLimitExceeded;
        result.error = "asset has too many external dependencies";
        return result;
    }

    struct Requirement
    {
        uint64_t maxBytes = 0;
        bool isImage = false;
    };
    std::map<std::string, Requirement> requirements;
    for (const GltfSourceDependency& dependency : dependencies)
    {
        const uint64_t maxBytes = dependency.kind == GltfDependencyKind::Image
            ? importLimits.maxEmbeddedImageBytes
            : importLimits.maxBufferBytes;
        const auto [it, inserted] = requirements.emplace(
            dependency.uri, Requirement{ maxBytes, dependency.kind == GltfDependencyKind::Image });
        if (!inserted)
        {
            it->second.maxBytes = (std::min)(it->second.maxBytes, maxBytes);
            it->second.isImage = it->second.isImage || dependency.kind == GltfDependencyKind::Image;
        }
    }
    if (requirements.size() > cookedLimits.maxDependencies)
    {
        result.status = SourceSnapshotStatus::ResourceLimitExceeded;
        result.error = "asset has too many unique external dependencies";
        return result;
    }

    for (const auto& [uri, requirement] : requirements)
    {
        std::vector<std::byte> bytes;
        const SourceSnapshotStatus readStatus =
            ReadBoundedFile(sourceDirectory / uri, requirement.maxBytes, bytes);
        if (readStatus != SourceSnapshotStatus::Success)
        {
            result.status = readStatus;
            result.error = "could not snapshot controlled asset dependency: " + uri;
            return result;
        }
        SourceDependencySnapshot snapshot;
        snapshot.uri = uri;
        snapshot.byteSize = bytes.size();
        snapshot.sha256 = ComputeSha256(bytes);
        snapshot.maxBytes = requirement.maxBytes;
        snapshot.isImage = requirement.isImage;
        if (snapshot.isImage)
            snapshot.imageBytes = std::move(bytes);
        result.dependencies.push_back(std::move(snapshot));
    }
    result.status = SourceSnapshotStatus::Success;
    return result;
}

SourceSnapshotStatus VerifySourceDependenciesUnchanged(
    const std::filesystem::path& sourceDirectory,
    const std::vector<SourceDependencySnapshot>& snapshots,
    std::string& error)
{
    for (const SourceDependencySnapshot& snapshot : snapshots)
    {
        std::vector<std::byte> bytes;
        const SourceSnapshotStatus readStatus =
            ReadBoundedFile(sourceDirectory / snapshot.uri, snapshot.maxBytes, bytes);
        if (readStatus != SourceSnapshotStatus::Success)
        {
            error = "could not verify controlled asset dependency: " + snapshot.uri;
            return readStatus;
        }
        if (bytes.size() != snapshot.byteSize || ComputeSha256(bytes) != snapshot.sha256)
        {
            error = "asset dependency changed during compilation: " + snapshot.uri;
            return SourceSnapshotStatus::Changed;
        }
    }
    error.clear();
    return SourceSnapshotStatus::Success;
}

} // namespace asset
