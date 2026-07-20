#include "test_framework.h"
#include "asset/cooked_model.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace
{

std::vector<std::byte> ToBytes(std::string_view text)
{
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

asset::Sha256Digest HashText(std::string_view text)
{
    return asset::ComputeSha256(ToBytes(text));
}

asset::ImportedModel BuildRepresentativeModel()
{
    asset::ImportedModel model;
    model.name = "stage3_representative";

    asset::ImportedSampler sampler;
    sampler.name = "paint_sampler";
    sampler.minFilter = asset::TextureFilter::LinearMipmapLinear;
    sampler.magFilter = asset::TextureFilter::Linear;
    sampler.wrapU = asset::TextureWrap::ClampToEdge;
    sampler.wrapV = asset::TextureWrap::MirroredRepeat;
    model.samplers.push_back(sampler);

    asset::ImageSource image;
    image.name = "paint_image";
    image.mimeType = "image/png";
    image.embeddedBytes = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
    model.images.push_back(image);

    asset::ImportedTexture texture;
    texture.name = "paint_texture";
    texture.imageIndex = 0;
    texture.samplerIndex = 0;
    model.textures.push_back(texture);

    asset::ImportedMaterial material;
    material.name = "painted_metal";
    material.baseColor = { 0.2f, 0.4f, 0.7f, 0.8f };
    material.roughness = 0.35f;
    material.metallic = 0.9f;
    material.emissive = { 0.03f, 0.04f, 0.05f, 1.0f };
    material.emissiveStrength = 2.5f;
    material.alphaCutoff = 0.42f;
    material.alphaMode = asset::AlphaMode::Mask;
    material.doubleSided = true;
    material.unlit = false;

    material.baseColorTexture.textureIndex = 0;
    material.baseColorTexture.texCoord = 1;
    material.baseColorTexture.strength = 0.8f;
    material.baseColorTexture.offset = { 0.1f, 0.2f };
    material.baseColorTexture.scale = { 1.5f, 0.75f };
    material.baseColorTexture.rotation = 0.25f;

    material.normalTexture.textureIndex = 0;
    material.normalTexture.texCoord = 0;
    material.normalTexture.strength = 0.65f;

    material.metallicRoughnessTexture.textureIndex = 0;
    material.metallicRoughnessTexture.texCoord = 1;

    material.occlusionTexture.textureIndex = 0;
    material.occlusionTexture.texCoord = 0;
    material.occlusionTexture.strength = 0.55f;

    material.emissiveTexture.textureIndex = 0;
    material.emissiveTexture.texCoord = 1;
    model.materials.push_back(material);

    asset::ImportedPrimitive primitive;
    primitive.name = "hull:0";
    primitive.nodeName = "HullNode";
    primitive.materialIndex = 0;

    asset::ImportedVertex a;
    a.position = { -1.0f, 0.0f, 2.0f };
    a.normal = { 0.0f, 0.0f, 1.0f };
    a.color = { 1.0f, 0.2f, 0.1f, 1.0f };
    a.uv0 = { 0.0f, 0.0f };
    a.uv1 = { 0.15f, 0.25f };
    a.texcoordMask = 3;
    a.tangent = { 1.0f, 0.0f, 0.0f, -1.0f };

    asset::ImportedVertex b;
    b.position = { 2.0f, 0.0f, 2.0f };
    b.normal = { 0.0f, 0.0f, 1.0f };
    b.color = { 0.1f, 1.0f, 0.3f, 0.75f };
    b.uv0 = { 1.0f, 0.0f };
    b.uv1 = { 0.85f, 0.25f };
    b.texcoordMask = 3;
    b.tangent = { 1.0f, 0.0f, 0.0f, -1.0f };

    asset::ImportedVertex c;
    c.position = { 0.0f, 3.0f, 2.0f };
    c.normal = { 0.0f, 0.0f, 1.0f };
    c.color = { 0.2f, 0.3f, 1.0f, 0.5f };
    c.uv0 = { 0.5f, 1.0f };
    c.uv1 = { 0.5f, 0.9f };
    c.texcoordMask = 3;
    c.tangent = { 1.0f, 0.0f, 0.0f, -1.0f };

    primitive.vertices = { a, b, c };
    primitive.indices = { 0, 1, 2 };
    for (const asset::ImportedVertex& vertex : primitive.vertices)
        primitive.bounds.Expand(vertex.position);
    model.bounds.Expand(primitive.bounds);
    model.primitives.push_back(primitive);
    return model;
}

asset::CookedModelMetadata BuildMetadata()
{
    asset::CookedModelMetadata metadata;
    metadata.sourceSha256 = HashText("source glb bytes");
    metadata.dependencies = {
        { "textures/zeta.png", 200, HashText("zeta dependency") },
        { "buffers/alpha.bin", 100, HashText("alpha dependency") }
    };
    return metadata;
}

void CheckVec2(const core::Vec2f& actual, const core::Vec2f& expected)
{
    CHECK_EQ(actual.x, expected.x);
    CHECK_EQ(actual.y, expected.y);
}

void CheckVec3(const core::Vec3f& actual, const core::Vec3f& expected)
{
    CHECK_EQ(actual.x, expected.x);
    CHECK_EQ(actual.y, expected.y);
    CHECK_EQ(actual.z, expected.z);
}

void CheckVec4(const core::Vec4f& actual, const core::Vec4f& expected)
{
    CHECK_EQ(actual.x, expected.x);
    CHECK_EQ(actual.y, expected.y);
    CHECK_EQ(actual.z, expected.z);
    CHECK_EQ(actual.w, expected.w);
}

void CheckColor(const core::Color& actual, const core::Color& expected)
{
    CHECK_EQ(actual.r, expected.r);
    CHECK_EQ(actual.g, expected.g);
    CHECK_EQ(actual.b, expected.b);
    CHECK_EQ(actual.a, expected.a);
}

void CheckBounds(const asset::Bounds3f& actual, const asset::Bounds3f& expected)
{
    CHECK_EQ(actual.valid, expected.valid);
    CheckVec3(actual.min, expected.min);
    CheckVec3(actual.max, expected.max);
}

void CheckBinding(
    const asset::TextureBinding& actual,
    const asset::TextureBinding& expected)
{
    CHECK_EQ(actual.textureIndex, expected.textureIndex);
    CHECK_EQ(actual.texCoord, expected.texCoord);
    CHECK_EQ(actual.strength, expected.strength);
    CheckVec2(actual.offset, expected.offset);
    CheckVec2(actual.scale, expected.scale);
    CHECK_EQ(actual.rotation, expected.rotation);
}

void CheckRoundTripModel(
    const asset::ImportedModel& actual,
    const asset::ImportedModel& expected)
{
    CHECK_EQ(actual.name, expected.name);
    CHECK_EQ(actual.VertexCount(), expected.VertexCount());
    CHECK_EQ(actual.IndexCount(), expected.IndexCount());
    CHECK_EQ(actual.primitives.size(), expected.primitives.size());
    CHECK_EQ(actual.materials.size(), expected.materials.size());
    CHECK_EQ(actual.images.size(), expected.images.size());
    CHECK_EQ(actual.samplers.size(), expected.samplers.size());
    CHECK_EQ(actual.textures.size(), expected.textures.size());
    CheckBounds(actual.bounds, expected.bounds);
    if (actual.primitives.size() != expected.primitives.size() ||
        actual.materials.size() != expected.materials.size() ||
        actual.images.size() != expected.images.size() ||
        actual.samplers.size() != expected.samplers.size() ||
        actual.textures.size() != expected.textures.size() ||
        actual.primitives.empty() || actual.materials.empty() || actual.images.empty() ||
        actual.samplers.empty() || actual.textures.empty())
    {
        return;
    }

    const asset::ImportedPrimitive& actualPrimitive = actual.primitives[0];
    const asset::ImportedPrimitive& expectedPrimitive = expected.primitives[0];
    CHECK_EQ(actualPrimitive.name, expectedPrimitive.name);
    CHECK_EQ(actualPrimitive.nodeName, expectedPrimitive.nodeName);
    CHECK_EQ(actualPrimitive.materialIndex, expectedPrimitive.materialIndex);
    CHECK_EQ(actualPrimitive.indices, expectedPrimitive.indices);
    CHECK_EQ(actualPrimitive.vertices.size(), expectedPrimitive.vertices.size());
    CheckBounds(actualPrimitive.bounds, expectedPrimitive.bounds);
    if (actualPrimitive.vertices.size() != expectedPrimitive.vertices.size())
        return;

    for (size_t i = 0; i < expectedPrimitive.vertices.size(); ++i)
    {
        const asset::ImportedVertex& actualVertex = actualPrimitive.vertices[i];
        const asset::ImportedVertex& expectedVertex = expectedPrimitive.vertices[i];
        CheckVec3(actualVertex.position, expectedVertex.position);
        CheckVec3(actualVertex.normal, expectedVertex.normal);
        CheckColor(actualVertex.color, expectedVertex.color);
        CheckVec2(actualVertex.uv0, expectedVertex.uv0);
        CheckVec2(actualVertex.uv1, expectedVertex.uv1);
        CHECK_EQ(actualVertex.texcoordMask, expectedVertex.texcoordMask);
        CheckVec4(actualVertex.tangent, expectedVertex.tangent);
    }

    const asset::ImportedMaterial& actualMaterial = actual.materials[0];
    const asset::ImportedMaterial& expectedMaterial = expected.materials[0];
    CHECK_EQ(actualMaterial.name, expectedMaterial.name);
    CheckColor(actualMaterial.baseColor, expectedMaterial.baseColor);
    CHECK_EQ(actualMaterial.roughness, expectedMaterial.roughness);
    CHECK_EQ(actualMaterial.metallic, expectedMaterial.metallic);
    CheckColor(actualMaterial.emissive, expectedMaterial.emissive);
    CHECK_EQ(actualMaterial.emissiveStrength, expectedMaterial.emissiveStrength);
    CHECK_EQ(actualMaterial.alphaCutoff, expectedMaterial.alphaCutoff);
    CHECK_EQ(actualMaterial.alphaMode, expectedMaterial.alphaMode);
    CHECK_EQ(actualMaterial.doubleSided, expectedMaterial.doubleSided);
    CHECK_EQ(actualMaterial.unlit, expectedMaterial.unlit);
    CheckBinding(actualMaterial.baseColorTexture, expectedMaterial.baseColorTexture);
    CheckBinding(actualMaterial.normalTexture, expectedMaterial.normalTexture);
    CheckBinding(
        actualMaterial.metallicRoughnessTexture,
        expectedMaterial.metallicRoughnessTexture);
    CheckBinding(actualMaterial.occlusionTexture, expectedMaterial.occlusionTexture);
    CheckBinding(actualMaterial.emissiveTexture, expectedMaterial.emissiveTexture);

    const asset::ImageSource& actualImage = actual.images[0];
    const asset::ImageSource& expectedImage = expected.images[0];
    CHECK_EQ(actualImage.name, expectedImage.name);
    CHECK_EQ(actualImage.uri, expectedImage.uri);
    CHECK_EQ(actualImage.mimeType, expectedImage.mimeType);
    CHECK_EQ(actualImage.embeddedBytes, expectedImage.embeddedBytes);

    const asset::ImportedSampler& actualSampler = actual.samplers[0];
    const asset::ImportedSampler& expectedSampler = expected.samplers[0];
    CHECK_EQ(actualSampler.name, expectedSampler.name);
    CHECK_EQ(actualSampler.minFilter, expectedSampler.minFilter);
    CHECK_EQ(actualSampler.magFilter, expectedSampler.magFilter);
    CHECK_EQ(actualSampler.wrapU, expectedSampler.wrapU);
    CHECK_EQ(actualSampler.wrapV, expectedSampler.wrapV);

    const asset::ImportedTexture& actualTexture = actual.textures[0];
    const asset::ImportedTexture& expectedTexture = expected.textures[0];
    CHECK_EQ(actualTexture.name, expectedTexture.name);
    CHECK_EQ(actualTexture.imageIndex, expectedTexture.imageIndex);
    CHECK_EQ(actualTexture.samplerIndex, expectedTexture.samplerIndex);
}

void WriteU32LittleEndian(std::vector<std::byte>& bytes, size_t offset, uint32_t value)
{
    bytes[offset + 0] = static_cast<std::byte>(value & 0xffu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
    bytes[offset + 2] = static_cast<std::byte>((value >> 16) & 0xffu);
    bytes[offset + 3] = static_cast<std::byte>((value >> 24) & 0xffu);
}

uint64_t ReadU64LittleEndian(const std::vector<std::byte>& bytes, size_t offset)
{
    uint64_t value = 0;
    for (uint32_t shift = 0; shift < 64; shift += 8)
        value |= static_cast<uint64_t>(std::to_integer<uint8_t>(bytes[offset++])) << shift;
    return value;
}

void WriteU64LittleEndian(std::vector<std::byte>& bytes, size_t offset, uint64_t value)
{
    for (uint32_t shift = 0; shift < 64; shift += 8)
        bytes[offset++] = static_cast<std::byte>(value >> shift);
}

void RefreshCookedCrc(std::vector<std::byte>& bytes)
{
    WriteU32LittleEndian(bytes, 24, 0);
    uint32_t crc = 0xffffffffu;
    for (std::byte byte : bytes)
    {
        crc ^= std::to_integer<uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    WriteU32LittleEndian(bytes, 24, ~crc);
}

} // namespace

TEST_CASE(AssetCompiler_ComputesKnownSha256Vector)
{
    CHECK_EQ(
        HashText("").Hex(),
        std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const asset::Sha256Digest digest = HashText("abc");
    CHECK_EQ(
        digest.Hex(),
        std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST_CASE(AssetCompiler_BuildIsDeterministicAndCanonicalizesDependencies)
{
    const asset::ImportedModel model = BuildRepresentativeModel();
    const asset::CookedModelMetadata metadata = BuildMetadata();

    const asset::CookedBuildResult first = asset::BuildCookedModel(model, metadata);
    const asset::CookedBuildResult second = asset::BuildCookedModel(model, metadata);
    CHECK(first.Succeeded());
    CHECK(second.Succeeded());
    if (!first.Succeeded() || !second.Succeeded())
        return;

    CHECK_FALSE(first.bytes.empty());
    CHECK_EQ(first.bytes, second.bytes);

    asset::CookedModelMetadata reordered = metadata;
    std::reverse(reordered.dependencies.begin(), reordered.dependencies.end());
    const asset::CookedBuildResult canonical = asset::BuildCookedModel(model, reordered);
    CHECK(canonical.Succeeded());
    if (canonical.Succeeded())
        CHECK_EQ(first.bytes, canonical.bytes);

    const asset::CookedModelResult loaded = asset::LoadCookedModelMemory(first.bytes);
    CHECK(loaded.Succeeded());
    if (!loaded.Succeeded())
        return;

    CHECK_EQ(loaded.metadata.dependencies.size(), size_t{ 2 });
    if (loaded.metadata.dependencies.size() != 2)
        return;
    CHECK_EQ(loaded.metadata.dependencies[0].uri, std::string("buffers/alpha.bin"));
    CHECK_EQ(loaded.metadata.dependencies[0].byteSize, uint64_t{ 100 });
    CHECK_EQ(loaded.metadata.dependencies[0].sha256, HashText("alpha dependency"));
    CHECK_EQ(loaded.metadata.dependencies[1].uri, std::string("textures/zeta.png"));
    CHECK_EQ(loaded.metadata.dependencies[1].byteSize, uint64_t{ 200 });
    CHECK_EQ(loaded.metadata.dependencies[1].sha256, HashText("zeta dependency"));
}

TEST_CASE(AssetCompiler_RoundTripsRepresentativeModelAndMetadata)
{
    const asset::ImportedModel expectedModel = BuildRepresentativeModel();
    const asset::CookedModelMetadata expectedMetadata = BuildMetadata();
    const asset::CookedBuildResult built =
        asset::BuildCookedModel(expectedModel, expectedMetadata);
    CHECK(built.Succeeded());
    CHECK(built.bytes.size() >= 12);
    if (!built.Succeeded() || built.bytes.size() < 12)
        return;

    const asset::CookedModelResult loaded = asset::LoadCookedModelMemory(built.bytes);
    CHECK(loaded.Succeeded());
    if (!loaded.Succeeded())
        return;

    CheckRoundTripModel(loaded.model, expectedModel);
    CHECK_EQ(loaded.metadata.sourceSha256, expectedMetadata.sourceSha256);
    CHECK_EQ(loaded.metadata.dependencies.size(), expectedMetadata.dependencies.size());
}

TEST_CASE(AssetCompiler_RejectsPayloadAndHeaderCrcCorruption)
{
    const asset::CookedBuildResult built =
        asset::BuildCookedModel(BuildRepresentativeModel(), BuildMetadata());
    CHECK(built.Succeeded());
    if (!built.Succeeded() || built.bytes.empty())
        return;

    std::vector<std::byte> corrupted = built.bytes;
    corrupted.back() ^= std::byte{ 0x01 };
    const asset::CookedModelResult loaded = asset::LoadCookedModelMemory(corrupted);
    CHECK_FALSE(loaded.Succeeded());
    CHECK_EQ(loaded.status, asset::CookedModelStatus::IntegrityMismatch);

    corrupted = built.bytes;
    // Source SHA-256 begins at byte 32; header identity is covered too.
    corrupted[32] ^= std::byte{ 0x01 };
    const asset::CookedModelResult headerLoaded =
        asset::LoadCookedModelMemory(corrupted);
    CHECK_FALSE(headerLoaded.Succeeded());
    CHECK_EQ(headerLoaded.status, asset::CookedModelStatus::IntegrityMismatch);
}

TEST_CASE(AssetCompiler_RejectsTruncatedBadMagicAndUnsupportedVersion)
{
    const asset::CookedBuildResult built =
        asset::BuildCookedModel(BuildRepresentativeModel(), BuildMetadata());
    CHECK(built.Succeeded());
    CHECK(built.bytes.size() >= 12);
    if (!built.Succeeded() || built.bytes.size() < 12)
        return;

    std::vector<std::byte> truncated = built.bytes;
    truncated.resize(truncated.size() / 2);
    const asset::CookedModelResult shortResult =
        asset::LoadCookedModelMemory(truncated);
    CHECK_FALSE(shortResult.Succeeded());

    std::vector<std::byte> badMagic = built.bytes;
    badMagic[0] ^= std::byte{ 0xff };
    const asset::CookedModelResult magicResult =
        asset::LoadCookedModelMemory(badMagic);
    CHECK_FALSE(magicResult.Succeeded());
    CHECK_EQ(magicResult.status, asset::CookedModelStatus::InvalidMagic);

    // The file header starts with an 8-byte magic followed by a LE uint32 version.
    std::vector<std::byte> badVersion = built.bytes;
    WriteU32LittleEndian(badVersion, 8, 0xffffffffu);
    const asset::CookedModelResult versionResult =
        asset::LoadCookedModelMemory(badVersion);
    CHECK_FALSE(versionResult.Succeeded());
    CHECK_EQ(versionResult.status, asset::CookedModelStatus::UnsupportedVersion);
}

TEST_CASE(AssetCompiler_EnforcesConfiguredLoadLimits)
{
    const asset::CookedBuildResult built =
        asset::BuildCookedModel(BuildRepresentativeModel(), BuildMetadata());
    CHECK(built.Succeeded());
    if (!built.Succeeded())
        return;

    asset::CookedModelLimits limits;
    limits.maxFileBytes = built.bytes.size() - 1;
    asset::CookedModelResult loaded =
        asset::LoadCookedModelMemory(built.bytes, limits);
    CHECK_FALSE(loaded.Succeeded());
    CHECK_EQ(loaded.status, asset::CookedModelStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxVertices = 2;
    loaded = asset::LoadCookedModelMemory(built.bytes, limits);
    CHECK_FALSE(loaded.Succeeded());
    CHECK_EQ(loaded.status, asset::CookedModelStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxEmbeddedImageBytes = 7;
    loaded = asset::LoadCookedModelMemory(built.bytes, limits);
    CHECK_FALSE(loaded.Succeeded());
    CHECK_EQ(loaded.status, asset::CookedModelStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxDependencies = 1;
    loaded = asset::LoadCookedModelMemory(built.bytes, limits);
    CHECK_FALSE(loaded.Succeeded());
    CHECK_EQ(loaded.status, asset::CookedModelStatus::ResourceLimitExceeded);
}

TEST_CASE(AssetCompiler_BuilderHonorsLoaderLimits)
{
    const asset::ImportedModel model = BuildRepresentativeModel();
    const asset::CookedModelMetadata metadata = BuildMetadata();

    asset::CookedModelLimits limits;
    limits.maxStringBytes = model.name.size() - 1;
    asset::CookedBuildResult built = asset::BuildCookedModel(model, metadata, limits);
    CHECK_FALSE(built.Succeeded());
    CHECK_EQ(built.status, asset::CookedModelStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxEmbeddedImageBytes = 7;
    built = asset::BuildCookedModel(model, metadata, limits);
    CHECK_FALSE(built.Succeeded());
    CHECK_EQ(built.status, asset::CookedModelStatus::ResourceLimitExceeded);

    const asset::CookedBuildResult baseline = asset::BuildCookedModel(model, metadata);
    CHECK(baseline.Succeeded());
    if (!baseline.Succeeded())
        return;
    limits = {};
    limits.maxFileBytes = baseline.bytes.size() - 1;
    built = asset::BuildCookedModel(model, metadata, limits);
    CHECK_FALSE(built.Succeeded());
    CHECK_EQ(built.status, asset::CookedModelStatus::ResourceLimitExceeded);
}

TEST_CASE(AssetCompiler_RejectsCountAllocationBombBeforeResize)
{
    const asset::CookedBuildResult built =
        asset::BuildCookedModel(BuildRepresentativeModel(), BuildMetadata());
    CHECK(built.Succeeded());
    if (!built.Succeeded())
        return;

    std::vector<std::byte> corrupted = built.bytes;
    // Geometry is the second 32-byte directory entry. Walk its two strings to
    // the first primitive's vertex count and make it large but policy-valid.
    size_t cursor = static_cast<size_t>(ReadU64LittleEndian(corrupted, 72 + 32 + 8));
    cursor += 8;
    for (int stringIndex = 0; stringIndex < 2; ++stringIndex)
    {
        const uint64_t length = ReadU64LittleEndian(corrupted, cursor);
        cursor += 8 + static_cast<size_t>(length);
    }
    cursor += 4 + 25;
    WriteU64LittleEndian(corrupted, cursor, 10'000'000);
    RefreshCookedCrc(corrupted);

    const asset::CookedModelResult loaded = asset::LoadCookedModelMemory(corrupted);
    CHECK_FALSE(loaded.Succeeded());
    CHECK_EQ(loaded.status, asset::CookedModelStatus::InvalidData);
}

TEST_CASE(AssetCompiler_InvalidAtomicWritePreservesExistingOutput)
{
    const asset::CookedBuildResult built =
        asset::BuildCookedModel(BuildRepresentativeModel(), BuildMetadata());
    CHECK(built.Succeeded());
    if (!built.Succeeded())
        return;

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "the_dawning_atomic_publish_test.tdmodel";
    std::error_code errorCode;
    std::filesystem::remove(output, errorCode);
    std::string error;
    CHECK_EQ(asset::WriteCookedModelFileAtomic(output, built.bytes, error),
             asset::CookedModelStatus::Success);

    std::vector<std::byte> corrupted = built.bytes;
    corrupted.back() ^= std::byte{ 0x01 };
    CHECK_EQ(asset::WriteCookedModelFileAtomic(output, corrupted, error),
             asset::CookedModelStatus::IntegrityMismatch);
    const asset::CookedModelResult preserved = asset::LoadCookedModelFile(output);
    CHECK(preserved.Succeeded());
    if (preserved.Succeeded())
        CHECK_EQ(preserved.metadata.sourceSha256, BuildMetadata().sourceSha256);
    std::filesystem::remove(output, errorCode);
}

TEST_CASE(AssetCompiler_RejectsInvalidModelBeforeWriting)
{
    asset::ImportedModel invalid = BuildRepresentativeModel();
    invalid.primitives[0].indices[2] = 99;

    const asset::CookedBuildResult built =
        asset::BuildCookedModel(invalid, BuildMetadata());
    CHECK_FALSE(built.Succeeded());
    CHECK_EQ(built.status, asset::CookedModelStatus::InvalidData);
    CHECK(built.bytes.empty());
}
