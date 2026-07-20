#include "source_snapshot.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

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

bool CheckedAdd(uint64_t left, uint64_t right, uint64_t& result)
{
    if (right > (std::numeric_limits<uint64_t>::max)() - left)
        return false;
    result = left + right;
    return true;
}

bool IsEmbeddedUri(const std::string& uri)
{
    return uri.empty() || std::string_view(uri).starts_with("data:");
}

bool PathsReferToSameFile(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    std::error_code errorCode;
    if (std::filesystem::exists(right, errorCode) && !errorCode &&
        std::filesystem::equivalent(left, right, errorCode) && !errorCode)
    {
        return true;
    }
    errorCode.clear();
    const std::filesystem::path canonicalLeft =
        std::filesystem::weakly_canonical(left, errorCode);
    if (errorCode)
        return false;
    const std::filesystem::path canonicalRight =
        std::filesystem::weakly_canonical(right, errorCode);
    if (errorCode)
        return false;
#if defined(_WIN32)
    const std::wstring leftText = canonicalLeft.native();
    const std::wstring rightText = canonicalRight.native();
    return CompareStringOrdinal(
               leftText.c_str(), -1, rightText.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return canonicalLeft == canonicalRight;
#endif
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
        bool isBuffer = false;
        bool isImage = false;
    };
    std::map<std::string, Requirement> requirements;
    for (const GltfSourceDependency& dependency : dependencies)
    {
        const uint64_t maxBytes = dependency.kind == GltfDependencyKind::Image
            ? importLimits.maxEmbeddedImageBytes
            : importLimits.maxBufferBytes;
        const auto [it, inserted] = requirements.emplace(
            dependency.uri,
            Requirement{
                maxBytes,
                dependency.kind == GltfDependencyKind::Buffer,
                dependency.kind == GltfDependencyKind::Image });
        if (!inserted)
        {
            it->second.maxBytes = (std::min)(it->second.maxBytes, maxBytes);
            it->second.isBuffer =
                it->second.isBuffer || dependency.kind == GltfDependencyKind::Buffer;
            it->second.isImage = it->second.isImage || dependency.kind == GltfDependencyKind::Image;
        }
    }
    if (requirements.size() > cookedLimits.maxDependencies)
    {
        result.status = SourceSnapshotStatus::ResourceLimitExceeded;
        result.error = "asset has too many unique external dependencies";
        return result;
    }

    const uint64_t imageBudget = (std::min)(
        importLimits.maxEmbeddedImageBytes,
        cookedLimits.maxEmbeddedImageBytes);
    const uint64_t bufferBudget = importLimits.maxBufferBytes;
    uint64_t totalImageBytes = 0;
    uint64_t totalBufferBytes = 0;
    for (const auto& [uri, requirement] : requirements)
    {
        std::vector<std::byte> bytes;
        uint64_t maxBytes = requirement.maxBytes;
        if (requirement.isImage)
        {
            if (totalImageBytes > imageBudget)
            {
                result.status = SourceSnapshotStatus::ResourceLimitExceeded;
                result.error = "external image snapshots exceed the configured aggregate limit";
                return result;
            }
            maxBytes = (std::min)(maxBytes, imageBudget - totalImageBytes);
        }
        if (requirement.isBuffer)
        {
            if (totalBufferBytes > bufferBudget)
            {
                result.status = SourceSnapshotStatus::ResourceLimitExceeded;
                result.error = "external buffer snapshots exceed the configured aggregate limit";
                return result;
            }
            maxBytes = (std::min)(maxBytes, bufferBudget - totalBufferBytes);
        }
        const SourceSnapshotStatus readStatus =
            ReadBoundedFile(sourceDirectory / uri, maxBytes, bytes);
        if (readStatus != SourceSnapshotStatus::Success)
        {
            result.status = readStatus;
            result.error = "could not snapshot controlled asset dependency: " + uri;
            return result;
        }
        if (requirement.isImage)
            totalImageBytes += static_cast<uint64_t>(bytes.size());
        if (requirement.isBuffer)
            totalBufferBytes += static_cast<uint64_t>(bytes.size());
        SourceDependencySnapshot snapshot;
        snapshot.uri = uri;
        snapshot.byteSize = bytes.size();
        snapshot.sha256 = ComputeSha256(bytes);
        snapshot.maxBytes = requirement.maxBytes;
        snapshot.isBuffer = requirement.isBuffer;
        snapshot.isImage = requirement.isImage;
        snapshot.payloadBytes = std::move(bytes);
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

SourceSnapshotStatus EmbedExternalImageSnapshots(
    ImportedModel& model,
    const std::vector<GltfSourceDependency>& dependencies,
    const std::vector<SourceDependencySnapshot>& snapshots,
    const GltfImportLimits& importLimits,
    const CookedModelLimits& cookedLimits,
    std::string& error)
{
    const uint64_t imageBudget = (std::min)(
        importLimits.maxEmbeddedImageBytes,
        cookedLimits.maxEmbeddedImageBytes);
    uint64_t totalImageBytes = 0;
    for (const ImageSource& image : model.images)
    {
        if (!IsEmbeddedUri(image.uri))
            continue;
        if (!CheckedAdd(
                totalImageBytes,
                static_cast<uint64_t>(image.embeddedBytes.size()),
                totalImageBytes) ||
            totalImageBytes > imageBudget)
        {
            error = "embedded image payloads exceed the configured aggregate limit";
            return SourceSnapshotStatus::ResourceLimitExceeded;
        }
    }

    std::vector<std::string_view> externalImageUris;
    try
    {
        externalImageUris.reserve(dependencies.size());
        for (const GltfSourceDependency& dependency : dependencies)
        {
            if (dependency.kind == GltfDependencyKind::Image)
                externalImageUris.push_back(dependency.uri);
        }
    }
    catch (const std::bad_alloc&)
    {
        error = "external image enumeration exceeds available memory";
        return SourceSnapshotStatus::ResourceLimitExceeded;
    }

    size_t externalImageIndex = 0;
    for (ImageSource& image : model.images)
    {
        if (IsEmbeddedUri(image.uri))
            continue;
        if (externalImageIndex >= externalImageUris.size())
        {
            error = "external image dependency enumeration is inconsistent";
            return SourceSnapshotStatus::InvalidData;
        }
        const std::string_view decodedUri = externalImageUris[externalImageIndex++];
        const auto snapshot = std::find_if(
            snapshots.begin(), snapshots.end(),
            [decodedUri](const SourceDependencySnapshot& candidate) {
                return candidate.uri == decodedUri && candidate.isImage;
            });
        if (snapshot == snapshots.end())
        {
            error = "external image snapshot is missing: " + std::string(decodedUri);
            return SourceSnapshotStatus::InvalidData;
        }

        uint64_t nextTotal = 0;
        if (!CheckedAdd(
                totalImageBytes,
                static_cast<uint64_t>(snapshot->payloadBytes.size()),
                nextTotal) ||
            nextTotal > imageBudget)
        {
            error = "materialized image payloads exceed the configured aggregate limit";
            return SourceSnapshotStatus::ResourceLimitExceeded;
        }
        try
        {
            image.embeddedBytes.resize(snapshot->payloadBytes.size());
        }
        catch (const std::bad_alloc&)
        {
            error = "materialized image payloads exceed available memory";
            return SourceSnapshotStatus::ResourceLimitExceeded;
        }
        if (!snapshot->payloadBytes.empty())
        {
            std::memcpy(
                image.embeddedBytes.data(), snapshot->payloadBytes.data(),
                snapshot->payloadBytes.size());
        }
        totalImageBytes = nextTotal;
    }
    if (externalImageIndex != externalImageUris.size())
    {
        error = "external image dependency enumeration is inconsistent";
        return SourceSnapshotStatus::InvalidData;
    }

    error.clear();
    return SourceSnapshotStatus::Success;
}

bool OutputPathAliasesSourceDependency(
    const std::filesystem::path& output,
    const std::filesystem::path& sourceDirectory,
    const std::vector<GltfSourceDependency>& dependencies,
    std::string& aliasedUri)
{
    for (const GltfSourceDependency& dependency : dependencies)
    {
        if (PathsReferToSameFile(sourceDirectory / dependency.uri, output))
        {
            aliasedUri = dependency.uri;
            return true;
        }
    }
    aliasedUri.clear();
    return false;
}

} // namespace asset
