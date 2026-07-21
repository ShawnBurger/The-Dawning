#pragma once

#include "assembly_resource_resolver.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace asset
{

constexpr uint32_t kRuntimeContentManifestVersion = 2u;

struct RuntimeContentBinding
{
    AssemblyResourceKind kind = AssemblyResourceKind::Visual;
    std::string locator;
    std::filesystem::path cookedModelPath;
    std::filesystem::path cookedCollisionPath;
    uint32_t primitiveIndex = 0;

    bool IsVisual() const { return kind == AssemblyResourceKind::Visual; }
    bool IsCollision() const { return kind == AssemblyResourceKind::Collision; }
};

struct RuntimeContentManifest
{
    uint32_t schemaVersion = 0;
    std::string sceneId;
    std::filesystem::path sourcePath;
    std::filesystem::path contentRoot;
    std::filesystem::path cookedAssemblyPath;
    AssemblyTransform rootTransform;
    std::vector<RuntimeContentBinding> bindings;
};

struct RuntimeContentManifestLimits
{
    uint64_t maxFileBytes = 1024ull * 1024ull;
    uint32_t maxLineBytes = 16u * 1024u;
    uint32_t maxStringBytes = 4096u;
    uint32_t maxBindings = 100'000u;
};

enum class RuntimeContentManifestStatus : uint8_t
{
    Success,
    FileNotFound,
    IoError,
    InvalidSyntax,
    UnsupportedVersion,
    InvalidData,
    ResourceLimitExceeded,
    AllocationFailure,
    InternalError
};

const char* RuntimeContentManifestStatusName(
    RuntimeContentManifestStatus status);

struct RuntimeContentManifestResult
{
    RuntimeContentManifestStatus status =
        RuntimeContentManifestStatus::InvalidData;
    RuntimeContentManifest manifest;
    std::string error;

    bool Succeeded() const
    {
        return status == RuntimeContentManifestStatus::Success;
    }
};

struct RuntimeContentCoverageResult
{
    bool matched = false;
    AssemblyResourceKind failedKind = AssemblyResourceKind::Visual;
    std::string failedLocator;
    std::string error;
};

RuntimeContentManifestResult ParseRuntimeContentManifest(
    std::string_view text,
    const RuntimeContentManifestLimits& limits = {});

RuntimeContentManifestResult LoadRuntimeContentManifestFile(
    const std::filesystem::path& path,
    const RuntimeContentManifestLimits& limits = {});

RuntimeContentCoverageResult ValidateRuntimeContentCoverage(
    const RuntimeContentManifest& manifest,
    const CookedAssembly& assembly);

} // namespace asset
