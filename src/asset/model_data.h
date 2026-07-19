#pragma once

#include "../core/types.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace asset
{

constexpr uint32_t kInvalidAssetIndex = UINT32_MAX;

struct Bounds3f
{
    core::Vec3f min = {
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)()
    };
    core::Vec3f max = {
        (std::numeric_limits<float>::lowest)(),
        (std::numeric_limits<float>::lowest)(),
        (std::numeric_limits<float>::lowest)()
    };
    bool valid = false;

    void Expand(const core::Vec3f& point)
    {
        min.x = (std::min)(min.x, point.x);
        min.y = (std::min)(min.y, point.y);
        min.z = (std::min)(min.z, point.z);
        max.x = (std::max)(max.x, point.x);
        max.y = (std::max)(max.y, point.y);
        max.z = (std::max)(max.z, point.z);
        valid = true;
    }

    void Expand(const Bounds3f& other)
    {
        if (!other.valid)
            return;
        Expand(other.min);
        Expand(other.max);
    }
};

enum class AlphaMode : uint8_t
{
    Opaque,
    Mask,
    Blend
};

struct TextureBinding
{
    uint32_t textureIndex = kInvalidAssetIndex;
    uint32_t texCoord = 0;
    float strength = 1.0f;
    core::Vec2f offset = { 0.0f, 0.0f };
    core::Vec2f scale = { 1.0f, 1.0f };
    float rotation = 0.0f;

    bool IsValid() const { return textureIndex != kInvalidAssetIndex; }
};

enum class TextureFilter : uint16_t
{
    Unspecified = 0,
    Nearest = 9728,
    Linear = 9729,
    NearestMipmapNearest = 9984,
    LinearMipmapNearest = 9985,
    NearestMipmapLinear = 9986,
    LinearMipmapLinear = 9987
};

enum class TextureWrap : uint16_t
{
    ClampToEdge = 33071,
    MirroredRepeat = 33648,
    Repeat = 10497
};

struct ImportedSampler
{
    std::string name;
    TextureFilter minFilter = TextureFilter::Unspecified;
    TextureFilter magFilter = TextureFilter::Unspecified;
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
};

struct ImportedTexture
{
    std::string name;
    uint32_t imageIndex = kInvalidAssetIndex;
    uint32_t samplerIndex = kInvalidAssetIndex;
};

struct ImageSource
{
    std::string name;
    std::string uri;
    std::string mimeType;
    std::vector<uint8_t> embeddedBytes;

    bool IsEmbedded() const { return !embeddedBytes.empty() || uri.starts_with("data:"); }
};

struct ImportedMaterial
{
    std::string name;
    core::Color baseColor = core::Color::White();
    float roughness = 1.0f;
    float metallic = 1.0f;
    core::Color emissive = { 0.0f, 0.0f, 0.0f, 1.0f };
    float emissiveStrength = 1.0f;
    float alphaCutoff = 0.5f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool doubleSided = false;
    bool unlit = false;

    TextureBinding baseColorTexture;
    TextureBinding normalTexture;
    TextureBinding metallicRoughnessTexture;
    TextureBinding occlusionTexture;
    TextureBinding emissiveTexture;
};

struct ImportedVertex
{
    core::Vec3f position;
    core::Vec3f normal;
    core::Color color = core::Color::White();
    core::Vec2f uv0;
    core::Vec2f uv1;
    uint8_t texcoordMask = 0;
    core::Vec4f tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
};

struct ImportedPrimitive
{
    std::string name;
    std::string nodeName;
    std::vector<ImportedVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t materialIndex = kInvalidAssetIndex;
    Bounds3f bounds;
};

struct ImportedModel
{
    std::string name;
    std::vector<ImportedPrimitive> primitives;
    std::vector<ImportedMaterial> materials;
    std::vector<ImageSource> images;
    std::vector<ImportedSampler> samplers;
    std::vector<ImportedTexture> textures;
    Bounds3f bounds;

    uint64_t VertexCount() const
    {
        uint64_t count = 0;
        for (const ImportedPrimitive& primitive : primitives)
            count += primitive.vertices.size();
        return count;
    }

    uint64_t IndexCount() const
    {
        uint64_t count = 0;
        for (const ImportedPrimitive& primitive : primitives)
            count += primitive.indices.size();
        return count;
    }
};

} // namespace asset
