#pragma once

#include "cooked_model.h"
#include "gltf_importer.h"

#include <filesystem>
#include <string>
#include <vector>

namespace asset
{

enum class SourceSnapshotStatus : uint8_t
{
    Success,
    IoError,
    ResourceLimitExceeded,
    Changed
};

struct SourceDependencySnapshot
{
    std::string uri;
    uint64_t byteSize = 0;
    Sha256Digest sha256;
    uint64_t maxBytes = 0;
    bool isImage = false;
    std::vector<std::byte> imageBytes;
};

struct SourceSnapshotResult
{
    SourceSnapshotStatus status = SourceSnapshotStatus::IoError;
    std::vector<SourceDependencySnapshot> dependencies;
    std::string error;

    bool Succeeded() const { return status == SourceSnapshotStatus::Success; }
};

SourceSnapshotResult CaptureSourceDependencies(
    const std::filesystem::path& sourceDirectory,
    const std::vector<GltfSourceDependency>& dependencies,
    const GltfImportLimits& importLimits = {},
    const CookedModelLimits& cookedLimits = {});

SourceSnapshotStatus VerifySourceDependenciesUnchanged(
    const std::filesystem::path& sourceDirectory,
    const std::vector<SourceDependencySnapshot>& snapshots,
    std::string& error);

} // namespace asset
