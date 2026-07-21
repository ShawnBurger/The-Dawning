#include "runtime_content_manifest.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>

namespace asset
{

namespace
{

RuntimeContentManifestResult Failure(
    RuntimeContentManifestStatus status,
    std::string error)
{
    RuntimeContentManifestResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

bool ValidLimits(const RuntimeContentManifestLimits& limits)
{
    return limits.maxFileBytes != 0 &&
           limits.maxLineBytes != 0 &&
           limits.maxStringBytes != 0 &&
           limits.maxBindings != 0;
}

bool IsSafeAscii(std::string_view value, uint32_t maxBytes)
{
    if (value.empty() || value.size() > maxBytes)
        return false;
    for (const unsigned char byte : value)
    {
        if (byte < 0x20u || byte > 0x7eu)
            return false;
    }
    return true;
}

bool IsValidSceneId(std::string_view value, uint32_t maxBytes)
{
    if (!IsSafeAscii(value, maxBytes))
        return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

bool IsValidRelativePath(std::string_view value, uint32_t maxBytes)
{
    if (!IsSafeAscii(value, maxBytes) || value.find('\\') != std::string_view::npos ||
        value.find(':') != std::string_view::npos)
        return false;

    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;

    bool hasComponent = false;
    for (const std::filesystem::path& component : path)
    {
        const std::string text = component.generic_string();
        if (text.empty() || text == "." || text == "..")
            return false;
        hasComponent = true;
    }
    return hasComponent;
}

bool LocatorMatchesKind(AssemblyResourceKind kind, std::string_view locator)
{
    switch (kind)
    {
    case AssemblyResourceKind::Visual:
        return locator.starts_with("source://") || locator.starts_with("visual://");
    case AssemblyResourceKind::Collision:
        return locator.starts_with("collision://");
    case AssemblyResourceKind::NavigationMesh:
        return locator.starts_with("nav://");
    case AssemblyResourceKind::WalkableSurface:
        return locator.starts_with("walk://");
    default:
        return false;
    }
}

bool ReadQuoted(std::istringstream& stream, std::string& value)
{
    stream >> std::ws;
    if (stream.peek() != '"')
        return false;
    stream >> std::quoted(value);
    return !stream.fail();
}

bool AtEnd(std::istringstream& stream)
{
    stream >> std::ws;
    return stream.eof();
}

std::string LineError(uint64_t line, std::string_view detail)
{
    return "line " + std::to_string(line) + ": " + std::string(detail);
}

bool AddBinding(
    RuntimeContentManifest& manifest,
    std::map<std::pair<uint8_t, std::string>, uint32_t>& identities,
    RuntimeContentBinding binding,
    const RuntimeContentManifestLimits& limits,
    uint64_t line,
    std::string& error)
{
    if (manifest.bindings.size() >= limits.maxBindings)
    {
        error = LineError(line, "binding limit exceeded");
        return false;
    }
    if (!IsSafeAscii(binding.locator, limits.maxStringBytes) ||
        !LocatorMatchesKind(binding.kind, binding.locator))
    {
        error = LineError(line, "locator is unsafe or has the wrong typed scheme");
        return false;
    }

    const auto key = std::make_pair(
        static_cast<uint8_t>(binding.kind), binding.locator);
    if (!identities.emplace(key, static_cast<uint32_t>(manifest.bindings.size())).second)
    {
        error = LineError(line, "duplicate typed locator");
        return false;
    }

    manifest.bindings.push_back(std::move(binding));
    return true;
}

} // namespace

const char* RuntimeContentManifestStatusName(
    RuntimeContentManifestStatus status)
{
    switch (status)
    {
    case RuntimeContentManifestStatus::Success: return "success";
    case RuntimeContentManifestStatus::FileNotFound: return "file not found";
    case RuntimeContentManifestStatus::IoError: return "I/O error";
    case RuntimeContentManifestStatus::InvalidSyntax: return "invalid syntax";
    case RuntimeContentManifestStatus::UnsupportedVersion: return "unsupported version";
    case RuntimeContentManifestStatus::InvalidData: return "invalid data";
    case RuntimeContentManifestStatus::ResourceLimitExceeded: return "resource limit exceeded";
    case RuntimeContentManifestStatus::AllocationFailure: return "allocation failure";
    case RuntimeContentManifestStatus::InternalError: return "internal error";
    default: return "unknown";
    }
}

RuntimeContentManifestResult ParseRuntimeContentManifest(
    std::string_view text,
    const RuntimeContentManifestLimits& limits)
{
    if (!ValidLimits(limits))
        return Failure(RuntimeContentManifestStatus::InvalidData, "invalid limits");
    if (text.empty())
        return Failure(RuntimeContentManifestStatus::InvalidSyntax, "empty manifest");
    if (text.size() > limits.maxFileBytes)
    {
        return Failure(RuntimeContentManifestStatus::ResourceLimitExceeded,
                       "manifest byte limit exceeded");
    }
    if (text.find('\0') != std::string_view::npos)
        return Failure(RuntimeContentManifestStatus::InvalidSyntax, "embedded NUL byte");

    try
    {
        RuntimeContentManifest manifest;
        std::map<std::pair<uint8_t, std::string>, uint32_t> identities;
        bool sawHeader = false;
        bool sawScene = false;
        bool sawAssembly = false;
        bool sawRoot = false;
        bool sawEnd = false;

        std::istringstream input{ std::string(text) };
        std::string lineText;
        uint64_t lineNumber = 0;
        while (std::getline(input, lineText))
        {
            ++lineNumber;
            if (!lineText.empty() && lineText.back() == '\r')
                lineText.pop_back();
            if (lineText.size() > limits.maxLineBytes)
            {
                return Failure(RuntimeContentManifestStatus::ResourceLimitExceeded,
                               LineError(lineNumber, "line byte limit exceeded"));
            }

            const size_t first = lineText.find_first_not_of(" \t");
            if (first == std::string::npos || lineText[first] == '#')
                continue;
            if (sawEnd)
            {
                return Failure(RuntimeContentManifestStatus::InvalidSyntax,
                               LineError(lineNumber, "content follows end marker"));
            }

            std::istringstream line(lineText);
            std::string directive;
            line >> directive;
            if (!sawHeader)
            {
                uint32_t version = 0;
                if (directive != "tdcontent" || !(line >> version) || !AtEnd(line))
                {
                    return Failure(RuntimeContentManifestStatus::InvalidSyntax,
                                   LineError(lineNumber, "expected 'tdcontent <version>'"));
                }
                if (version != kRuntimeContentManifestVersion)
                {
                    return Failure(RuntimeContentManifestStatus::UnsupportedVersion,
                                   LineError(lineNumber, "unsupported schema version"));
                }
                manifest.schemaVersion = version;
                sawHeader = true;
                continue;
            }

            if (directive == "scene")
            {
                if (sawScene || !ReadQuoted(line, manifest.sceneId) || !AtEnd(line) ||
                    !IsValidSceneId(manifest.sceneId, limits.maxStringBytes))
                {
                    return Failure(RuntimeContentManifestStatus::InvalidData,
                                   LineError(lineNumber, "invalid or duplicate scene id"));
                }
                sawScene = true;
            }
            else if (directive == "assembly")
            {
                std::string path;
                if (sawAssembly || !ReadQuoted(line, path) || !AtEnd(line) ||
                    !IsValidRelativePath(path, limits.maxStringBytes))
                {
                    return Failure(RuntimeContentManifestStatus::InvalidData,
                                   LineError(lineNumber, "invalid or duplicate assembly path"));
                }
                manifest.cookedAssemblyPath = std::filesystem::path(path);
                sawAssembly = true;
            }
            else if (directive == "root")
            {
                AssemblyTransform root;
                if (sawRoot ||
                    !(line >> root.positionMeters[0] >> root.positionMeters[1] >>
                      root.positionMeters[2] >> root.rotationEulerDegrees[0] >>
                      root.rotationEulerDegrees[1] >> root.rotationEulerDegrees[2] >>
                      root.scale[0] >> root.scale[1] >> root.scale[2]) ||
                    !AtEnd(line))
                {
                    return Failure(RuntimeContentManifestStatus::InvalidSyntax,
                                   LineError(lineNumber, "invalid or duplicate root transform"));
                }
                for (double value : root.positionMeters)
                    if (!std::isfinite(value))
                        return Failure(RuntimeContentManifestStatus::InvalidData,
                                       LineError(lineNumber, "non-finite root position"));
                for (double value : root.rotationEulerDegrees)
                    if (!std::isfinite(value))
                        return Failure(RuntimeContentManifestStatus::InvalidData,
                                       LineError(lineNumber, "non-finite root rotation"));
                for (double value : root.scale)
                    if (!std::isfinite(value) || value <= 0.0)
                        return Failure(RuntimeContentManifestStatus::InvalidData,
                                       LineError(lineNumber, "invalid root scale"));
                manifest.rootTransform = root;
                sawRoot = true;
            }
            else if (directive == "visual")
            {
                RuntimeContentBinding binding;
                binding.kind = AssemblyResourceKind::Visual;
                std::string path;
                if (!ReadQuoted(line, binding.locator) || !ReadQuoted(line, path) ||
                    !(line >> binding.primitiveIndex) || !AtEnd(line) ||
                    !IsValidRelativePath(path, limits.maxStringBytes))
                {
                    return Failure(RuntimeContentManifestStatus::InvalidData,
                                   LineError(lineNumber, "invalid visual binding"));
                }
                binding.cookedModelPath = std::filesystem::path(path);
                std::string error;
                if (!AddBinding(manifest, identities, std::move(binding), limits,
                                lineNumber, error))
                {
                    const RuntimeContentManifestStatus status =
                        manifest.bindings.size() >= limits.maxBindings
                            ? RuntimeContentManifestStatus::ResourceLimitExceeded
                            : RuntimeContentManifestStatus::InvalidData;
                    return Failure(status, std::move(error));
                }
            }
            else if (directive == "collision" || directive == "navigation" ||
                     directive == "walkable")
            {
                RuntimeContentBinding binding;
                binding.kind = directive == "collision"
                    ? AssemblyResourceKind::Collision
                    : directive == "navigation"
                        ? AssemblyResourceKind::NavigationMesh
                        : AssemblyResourceKind::WalkableSurface;
                if (!ReadQuoted(line, binding.locator) || !AtEnd(line))
                {
                    return Failure(RuntimeContentManifestStatus::InvalidSyntax,
                                   LineError(lineNumber, "invalid contract binding"));
                }
                std::string error;
                if (!AddBinding(manifest, identities, std::move(binding), limits,
                                lineNumber, error))
                {
                    const RuntimeContentManifestStatus status =
                        manifest.bindings.size() >= limits.maxBindings
                            ? RuntimeContentManifestStatus::ResourceLimitExceeded
                            : RuntimeContentManifestStatus::InvalidData;
                    return Failure(status, std::move(error));
                }
            }
            else if (directive == "end")
            {
                if (!AtEnd(line))
                {
                    return Failure(RuntimeContentManifestStatus::InvalidSyntax,
                                   LineError(lineNumber, "end marker has trailing data"));
                }
                sawEnd = true;
            }
            else
            {
                return Failure(RuntimeContentManifestStatus::InvalidSyntax,
                               LineError(lineNumber, "unknown directive"));
            }
        }

        if (!sawHeader || !sawScene || !sawAssembly || !sawRoot || !sawEnd ||
            manifest.bindings.empty())
        {
            return Failure(RuntimeContentManifestStatus::InvalidData,
                           "manifest is missing required records");
        }

        RuntimeContentManifestResult result;
        result.status = RuntimeContentManifestStatus::Success;
        result.manifest = std::move(manifest);
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return Failure(RuntimeContentManifestStatus::AllocationFailure,
                       "allocation failure");
    }
    catch (...)
    {
        return Failure(RuntimeContentManifestStatus::InternalError,
                       "unexpected parser failure");
    }
}

RuntimeContentManifestResult LoadRuntimeContentManifestFile(
    const std::filesystem::path& path,
    const RuntimeContentManifestLimits& limits)
{
    if (!ValidLimits(limits) || path.empty())
        return Failure(RuntimeContentManifestStatus::InvalidData, "invalid arguments");

    try
    {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error)
            return Failure(RuntimeContentManifestStatus::IoError, "unable to query manifest");
        if (!exists)
            return Failure(RuntimeContentManifestStatus::FileNotFound, "manifest not found");

        const uintmax_t fileBytes = std::filesystem::file_size(path, error);
        if (error)
            return Failure(RuntimeContentManifestStatus::IoError, "unable to size manifest");
        if (fileBytes == 0)
            return Failure(RuntimeContentManifestStatus::InvalidSyntax, "empty manifest");
        if (fileBytes > limits.maxFileBytes ||
            fileBytes > static_cast<uintmax_t>((std::numeric_limits<size_t>::max)()))
        {
            return Failure(RuntimeContentManifestStatus::ResourceLimitExceeded,
                           "manifest byte limit exceeded");
        }

        std::string text(static_cast<size_t>(fileBytes), '\0');
        std::ifstream input(path, std::ios::binary);
        if (!input || !input.read(text.data(), static_cast<std::streamsize>(text.size())))
            return Failure(RuntimeContentManifestStatus::IoError, "unable to read manifest");
        if (input.peek() != std::char_traits<char>::eof())
            return Failure(RuntimeContentManifestStatus::IoError, "manifest changed while reading");

        RuntimeContentManifestResult result =
            ParseRuntimeContentManifest(text, limits);
        if (result.Succeeded())
        {
            result.manifest.sourcePath = std::filesystem::absolute(path).lexically_normal();
            result.manifest.contentRoot = result.manifest.sourcePath.parent_path();
        }
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return Failure(RuntimeContentManifestStatus::AllocationFailure,
                       "allocation failure");
    }
    catch (const std::filesystem::filesystem_error&)
    {
        return Failure(RuntimeContentManifestStatus::IoError,
                       "filesystem error while reading manifest");
    }
    catch (...)
    {
        return Failure(RuntimeContentManifestStatus::InternalError,
                       "unexpected file-loader failure");
    }
}

RuntimeContentCoverageResult ValidateRuntimeContentCoverage(
    const RuntimeContentManifest& manifest,
    const CookedAssembly& assembly)
{
    RuntimeContentCoverageResult result;
    try
    {
        using Key = std::pair<uint8_t, std::string>;
        std::map<Key, AssemblyResourceKind> expected;
        const auto addExpected = [&](AssemblyResourceKind kind,
                                     const std::string& locator) -> bool {
            if (locator.empty())
                return false;
            expected.emplace(
                std::make_pair(static_cast<uint8_t>(kind), locator), kind);
            return true;
        };

        for (const AssemblyModule& module : assembly.modules)
        {
            if (!addExpected(AssemblyResourceKind::Visual, module.visualSource) ||
                !addExpected(AssemblyResourceKind::Collision, module.collisionSource))
            {
                result.error = "assembly contains an empty module locator";
                return result;
            }
            for (const AssemblyLod& lod : module.lods)
            {
                if (!addExpected(AssemblyResourceKind::Visual, lod.source))
                {
                    result.error = "assembly contains an empty LOD locator";
                    return result;
                }
            }
        }
        for (const AssemblyZone& zone : assembly.zones)
        {
            if (!addExpected(
                    AssemblyResourceKind::NavigationMesh, zone.navmeshSource) ||
                !addExpected(
                    AssemblyResourceKind::WalkableSurface, zone.walkableSurface))
            {
                result.error = "assembly contains an empty zone locator";
                return result;
            }
        }
        for (const AssemblyMovingPart& part : assembly.movingParts)
        {
            if (!addExpected(AssemblyResourceKind::Visual, part.visualSource))
            {
                result.error = "assembly contains an empty moving-part locator";
                return result;
            }
        }

        std::map<Key, AssemblyResourceKind> actual;
        for (const RuntimeContentBinding& binding : manifest.bindings)
        {
            const Key key{
                static_cast<uint8_t>(binding.kind), binding.locator };
            if (!actual.emplace(key, binding.kind).second)
            {
                result.failedKind = binding.kind;
                result.failedLocator = binding.locator;
                result.error = "runtime content contains a duplicate typed locator";
                return result;
            }
        }

        for (const auto& [key, kind] : expected)
        {
            if (actual.find(key) == actual.end())
            {
                result.failedKind = kind;
                result.failedLocator = key.second;
                result.error = "runtime content is missing an authored locator";
                return result;
            }
        }
        for (const auto& [key, kind] : actual)
        {
            if (expected.find(key) == expected.end())
            {
                result.failedKind = kind;
                result.failedLocator = key.second;
                result.error = "runtime content contains an unauthored locator";
                return result;
            }
        }

        result.matched = true;
        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.error = "allocation failure during coverage validation";
        return result;
    }
    catch (...)
    {
        result.error = "unexpected coverage-validation failure";
        return result;
    }
}

} // namespace asset
