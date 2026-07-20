#pragma once

#include "model_data.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace asset
{

struct Sha256Digest
{
    std::array<uint8_t, 32> bytes{};

    bool operator==(const Sha256Digest&) const = default;
    std::string Hex() const;
};

Sha256Digest ComputeSha256(std::span<const std::byte> bytes);

struct CookedDependency
{
    std::string uri;
    uint64_t byteSize = 0;
    Sha256Digest sha256;
};

struct CookedModelMetadata
{
    Sha256Digest sourceSha256;
    std::vector<CookedDependency> dependencies;
};

enum class CookedModelStatus : uint8_t
{
    Success,
    FileNotFound,
    IoError,
    InvalidMagic,
    UnsupportedVersion,
    InvalidLayout,
    IntegrityMismatch,
    ResourceLimitExceeded,
    InvalidData
};

const char* CookedModelStatusName(CookedModelStatus status);

struct CookedModelLimits
{
    uint64_t maxFileBytes = 2ull * 1024ull * 1024ull * 1024ull;
    uint64_t maxSectionBytes = 1024ull * 1024ull * 1024ull;
    uint64_t maxStringBytes = 1024ull * 1024ull;
    uint64_t maxEmbeddedImageBytes = 512ull * 1024ull * 1024ull;
    uint64_t maxVertices = 10'000'000ull;
    uint64_t maxIndices = 30'000'000ull;
    uint64_t maxPrimitives = 100'000ull;
    uint64_t maxMaterials = 100'000ull;
    uint64_t maxImages = 100'000ull;
    uint64_t maxSamplers = 100'000ull;
    uint64_t maxTextures = 100'000ull;
    uint64_t maxDependencies = 100'000ull;
};

struct CookedBuildResult
{
    CookedModelStatus status = CookedModelStatus::InvalidData;
    std::vector<std::byte> bytes;
    std::string error;

    bool Succeeded() const { return status == CookedModelStatus::Success; }
};

struct CookedModelResult
{
    CookedModelStatus status = CookedModelStatus::InvalidData;
    ImportedModel model;
    CookedModelMetadata metadata;
    std::string error;

    bool Succeeded() const { return status == CookedModelStatus::Success; }
};

CookedBuildResult BuildCookedModel(
    const ImportedModel& model,
    const CookedModelMetadata& metadata = {},
    const CookedModelLimits& limits = {});

CookedModelResult LoadCookedModelMemory(
    std::span<const std::byte> bytes,
    const CookedModelLimits& limits = {});

CookedModelResult LoadCookedModelFile(
    const std::filesystem::path& path,
    const CookedModelLimits& limits = {});

CookedModelStatus WriteCookedModelFileAtomic(
    const std::filesystem::path& path,
    std::span<const std::byte> bytes,
    std::string& error);

} // namespace asset
