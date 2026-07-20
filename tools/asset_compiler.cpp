#include "asset/cooked_model.h"
#include "asset/gltf_importer.h"
#include "asset/source_snapshot.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{

bool ReadFile(
    const std::filesystem::path& path,
    uint64_t maxBytes,
    std::vector<std::byte>& bytes)
{
    std::error_code errorCode;
    const uint64_t size = std::filesystem::file_size(path, errorCode);
    if (errorCode || size > maxBytes || size > (std::numeric_limits<size_t>::max)() ||
        size > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)()))
        return false;
    try
    {
        bytes.resize(static_cast<size_t>(size));
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    return static_cast<bool>(stream) &&
           static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()),
                                         static_cast<std::streamsize>(bytes.size())));
}

bool PathsReferToSameFile(
    const std::filesystem::path& source,
    const std::filesystem::path& output)
{
    std::error_code errorCode;
    if (std::filesystem::exists(output, errorCode) && !errorCode &&
        std::filesystem::equivalent(source, output, errorCode) && !errorCode)
    {
        return true;
    }
    errorCode.clear();
    const std::filesystem::path canonicalSource =
        std::filesystem::weakly_canonical(source, errorCode);
    if (errorCode)
        return false;
    const std::filesystem::path canonicalOutput =
        std::filesystem::weakly_canonical(output, errorCode);
    if (errorCode)
        return false;
#if defined(_WIN32)
    const std::wstring left = canonicalSource.native();
    const std::wstring right = canonicalOutput.native();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return canonicalSource == canonicalOutput;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        std::cerr << "usage: TheDawningAssetCompiler <source.gltf|source.glb> [output.tdmodel]\n";
        return 2;
    }

    const std::filesystem::path sourcePath = argv[1];
    std::filesystem::path outputPath = argc == 3 ? std::filesystem::path(argv[2]) : sourcePath;
    if (argc != 3)
        outputPath.replace_extension(".tdmodel");
    const asset::GltfImportLimits importLimits;
    const asset::CookedModelLimits cookedLimits;
    std::error_code pathError;
    const std::filesystem::path absoluteSource =
        std::filesystem::absolute(sourcePath, pathError).lexically_normal();
    if (pathError)
    {
        std::cerr << "could not resolve source asset path\n";
        return 2;
    }
    pathError.clear();
    const std::filesystem::path absoluteOutput =
        std::filesystem::absolute(outputPath, pathError).lexically_normal();
    if (pathError)
    {
        std::cerr << "could not resolve cooked output path\n";
        return 2;
    }
    if (PathsReferToSameFile(absoluteSource, absoluteOutput))
    {
        std::cerr << "source and cooked output paths must differ\n";
        return 2;
    }

    std::vector<std::byte> sourceBytes;
    if (!ReadFile(sourcePath, importLimits.maxSourceBytes, sourceBytes))
    {
        std::cerr << "could not read source asset\n";
        return 1;
    }

    const asset::GltfDependencyScanResult scanned =
        asset::ScanGltfSourceDependencies(sourceBytes, importLimits);
    if (!scanned.Succeeded())
    {
        std::cerr << "dependency scan failed ["
                  << asset::GltfImportStatusName(scanned.status) << "]: "
                  << scanned.error << '\n';
        return 1;
    }
    std::string aliasedDependency;
    if (asset::OutputPathAliasesSourceDependency(
            absoluteOutput, sourcePath.parent_path(), scanned.dependencies,
            aliasedDependency))
    {
        std::cerr << "cooked output path aliases source dependency: "
                  << aliasedDependency << '\n';
        return 2;
    }
    const asset::SourceSnapshotResult snapshots = asset::CaptureSourceDependencies(
        sourcePath.parent_path(), scanned.dependencies, importLimits, cookedLimits);
    if (!snapshots.Succeeded())
    {
        std::cerr << "dependency snapshot failed: " << snapshots.error << '\n';
        return 1;
    }

    std::vector<asset::GltfExternalBuffer> externalBuffers;
    try
    {
        externalBuffers.reserve(snapshots.dependencies.size());
        for (const asset::SourceDependencySnapshot& snapshot : snapshots.dependencies)
        {
            if (snapshot.isBuffer)
            {
                externalBuffers.push_back({
                    snapshot.uri,
                    std::span<const std::byte>(snapshot.payloadBytes)
                });
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        std::cerr << "external buffer snapshot table exceeds available memory\n";
        return 1;
    }

    asset::GltfImportResult imported = asset::ImportGltfMemoryWithExternalBuffers(
        sourceBytes, sourcePath, externalBuffers, importLimits);
    if (!imported.Succeeded())
    {
        std::cerr << "import failed [" << asset::GltfImportStatusName(imported.status)
                  << "]: " << imported.error << '\n';
        return 1;
    }

    asset::CookedModelMetadata metadata;
    metadata.sourceSha256 = asset::ComputeSha256(sourceBytes);
    for (const asset::SourceDependencySnapshot& snapshot : snapshots.dependencies)
    {
        metadata.dependencies.push_back({
            snapshot.uri,
            snapshot.byteSize,
            snapshot.sha256
        });
    }

    // External images become self-contained cooked payloads while retaining
    // their URI as dependency identity for incremental rebuilds.
    std::string imageSnapshotError;
    if (asset::EmbedExternalImageSnapshots(
            imported.model, scanned.dependencies, snapshots.dependencies,
            importLimits, cookedLimits, imageSnapshotError) !=
        asset::SourceSnapshotStatus::Success)
    {
        std::cerr << "external image embedding failed: " << imageSnapshotError << '\n';
        return 1;
    }

    const asset::CookedBuildResult cooked =
        asset::BuildCookedModel(imported.model, metadata, cookedLimits);
    if (!cooked.Succeeded())
    {
        std::cerr << "cook failed [" << asset::CookedModelStatusName(cooked.status)
                  << "]: " << cooked.error << '\n';
        return 1;
    }

    std::string snapshotError;
    if (asset::VerifySourceDependenciesUnchanged(
            sourcePath.parent_path(), snapshots.dependencies, snapshotError) !=
        asset::SourceSnapshotStatus::Success)
    {
        std::cerr << "dependency verification failed: " << snapshotError << '\n';
        return 1;
    }

    std::string writeError;
    if (PathsReferToSameFile(absoluteSource, absoluteOutput) ||
        asset::OutputPathAliasesSourceDependency(
            absoluteOutput, sourcePath.parent_path(), scanned.dependencies,
            aliasedDependency))
    {
        std::cerr << "cooked output became an alias of source input before publication\n";
        return 2;
    }
    const asset::CookedModelStatus writeStatus =
        asset::WriteCookedModelFileAtomic(outputPath, cooked.bytes, writeError, cookedLimits);
    if (writeStatus != asset::CookedModelStatus::Success)
    {
        std::cerr << "write failed [" << asset::CookedModelStatusName(writeStatus)
                  << "]: " << writeError << '\n';
        return 1;
    }

    const asset::CookedModelResult verified = asset::LoadCookedModelFile(outputPath);
    if (!verified.Succeeded() || verified.metadata.sourceSha256 != metadata.sourceSha256 ||
        verified.model.VertexCount() != imported.model.VertexCount() ||
        verified.model.IndexCount() != imported.model.IndexCount())
    {
        std::cerr << "verification of the written cooked model failed";
        if (!verified.error.empty())
            std::cerr << ": " << verified.error;
        std::cerr << '\n';
        return 1;
    }

    std::cout << "cooked=" << outputPath.string() << '\n';
    std::cout << "source_sha256=" << metadata.sourceSha256.Hex() << '\n';
    std::cout << "bytes=" << cooked.bytes.size() << '\n';
    std::cout << "primitives=" << verified.model.primitives.size() << '\n';
    std::cout << "vertices=" << verified.model.VertexCount() << '\n';
    std::cout << "triangles=" << verified.model.IndexCount() / 3 << '\n';
    std::cout << "dependencies=" << verified.metadata.dependencies.size() << '\n';
    return 0;
}
