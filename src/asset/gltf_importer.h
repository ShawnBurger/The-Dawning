#pragma once

#include "model_data.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace asset
{

enum class GltfImportStatus : uint8_t
{
    Success,
    FileNotFound,
    IoError,
    SourceTooLarge,
    ParseError,
    BufferLoadError,
    ValidationError,
    UnsupportedFeature,
    InvalidGeometry,
    ResourceLimitExceeded
};

const char* GltfImportStatusName(GltfImportStatus status);

struct GltfImportLimits
{
    uint64_t maxSourceBytes = 512ull * 1024ull * 1024ull;
    uint64_t maxBufferBytes = 1024ull * 1024ull * 1024ull;
    uint64_t maxEmbeddedImageBytes = 512ull * 1024ull * 1024ull;
    uint64_t maxVertices = 10'000'000ull;
    uint64_t maxIndices = 30'000'000ull;
    uint64_t maxPrimitives = 100'000ull;
    uint32_t maxNodeDepth = 256;
};

enum class GltfDependencyKind : uint8_t
{
    Buffer,
    Image
};

struct GltfSourceDependency
{
    std::string uri;
    GltfDependencyKind kind = GltfDependencyKind::Buffer;
};

struct GltfDependencyScanResult
{
    GltfImportStatus status = GltfImportStatus::ParseError;
    std::vector<GltfSourceDependency> dependencies;
    std::string error;

    bool Succeeded() const { return status == GltfImportStatus::Success; }
};

struct GltfImportResult
{
    GltfImportStatus status = GltfImportStatus::ParseError;
    ImportedModel model;
    std::vector<GltfSourceDependency> sourceDependencies;
    std::vector<std::string> warnings;
    std::string error;

    bool Succeeded() const { return status == GltfImportStatus::Success; }
};

struct GltfExternalBuffer
{
    std::string_view uri;
    std::span<const std::byte> bytes;
};

GltfImportResult ImportGltfFile(
    const std::filesystem::path& path,
    const GltfImportLimits& limits = {});

// Primarily used by tests and offline tooling. virtualSourcePath gives cgltf a
// base directory for external buffer URIs; GLB and data-URI buffers need none.
GltfImportResult ImportGltfMemory(
    std::span<const std::byte> bytes,
    const std::filesystem::path& virtualSourcePath = {},
    const GltfImportLimits& limits = {});

// Offline cooking uses this strict entry point so cgltf consumes the captured
// dependency bytes instead of reopening mutable files from disk.
GltfImportResult ImportGltfMemoryWithExternalBuffers(
    std::span<const std::byte> bytes,
    const std::filesystem::path& virtualSourcePath,
    std::span<const GltfExternalBuffer> externalBuffers,
    const GltfImportLimits& limits = {});

GltfDependencyScanResult ScanGltfSourceDependencies(
    std::span<const std::byte> bytes,
    const GltfImportLimits& limits = {});

} // namespace asset
