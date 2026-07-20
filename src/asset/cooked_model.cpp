#include "cooked_model.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <thread>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace asset
{
namespace
{

constexpr std::array<uint8_t, 8> kMagic = { 'T', 'D', 'M', 'O', 'D', 'E', 'L', 0 };
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kSectionVersion = 1;
constexpr uint32_t kSectionCount = 6;
constexpr uint64_t kFixedHeaderBytes = 72;
constexpr uint64_t kSectionEntryBytes = 32;

enum class SectionType : uint32_t
{
    Metadata = 1,
    Geometry = 2,
    Materials = 3,
    Images = 4,
    Samplers = 5,
    Textures = 6
};

struct Section
{
    SectionType type{};
    uint64_t elementCount = 0;
    std::vector<std::byte> bytes;
    uint64_t offset = 0;
};

struct SectionView
{
    SectionType type{};
    uint64_t elementCount = 0;
    std::span<const std::byte> bytes;
    uint64_t offset = 0;
};

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    if (alignment == 0 || value > (std::numeric_limits<uint64_t>::max)() - (alignment - 1))
        return 0;
    return (value + alignment - 1) & ~(alignment - 1);
}

bool CheckedAdd(uint64_t left, uint64_t right, uint64_t& result)
{
    if (right > (std::numeric_limits<uint64_t>::max)() - left)
        return false;
    result = left + right;
    return true;
}

bool CheckedMultiply(uint64_t left, uint64_t right, uint64_t& result)
{
    if (left != 0 && right > (std::numeric_limits<uint64_t>::max)() / left)
        return false;
    result = left * right;
    return true;
}

bool IsFinite(float value)
{
    return std::isfinite(value);
}

bool IsFinite(const core::Vec2f& value)
{
    return IsFinite(value.x) && IsFinite(value.y);
}

bool IsFinite(const core::Vec3f& value)
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

bool IsFinite(const core::Vec4f& value)
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) && IsFinite(value.w);
}

bool IsFinite(const core::Color& value)
{
    return IsFinite(value.r) && IsFinite(value.g) && IsFinite(value.b) && IsFinite(value.a);
}

bool IsUnit(float value)
{
    return value >= 0.0f && value <= 1.0f;
}

bool IsUnit(const core::Color& value)
{
    return IsUnit(value.r) && IsUnit(value.g) && IsUnit(value.b) && IsUnit(value.a);
}

bool IsOrderedFinite(const Bounds3f& bounds)
{
    return bounds.valid && IsFinite(bounds.min) && IsFinite(bounds.max) &&
           bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y &&
           bounds.min.z <= bounds.max.z;
}

bool IsValidTextureFilter(TextureFilter filter)
{
    switch (filter)
    {
    case TextureFilter::Unspecified:
    case TextureFilter::Nearest:
    case TextureFilter::Linear:
    case TextureFilter::NearestMipmapNearest:
    case TextureFilter::LinearMipmapNearest:
    case TextureFilter::NearestMipmapLinear:
    case TextureFilter::LinearMipmapLinear:
        return true;
    }
    return false;
}

bool IsValidTextureWrap(TextureWrap wrap)
{
    return wrap == TextureWrap::ClampToEdge || wrap == TextureWrap::MirroredRepeat ||
           wrap == TextureWrap::Repeat;
}

class Writer
{
public:
    void U8(uint8_t value) { m_bytes.push_back(static_cast<std::byte>(value)); }

    void U16(uint16_t value)
    {
        U8(static_cast<uint8_t>(value));
        U8(static_cast<uint8_t>(value >> 8));
    }

    void U32(uint32_t value)
    {
        for (uint32_t shift = 0; shift < 32; shift += 8)
            U8(static_cast<uint8_t>(value >> shift));
    }

    void U64(uint64_t value)
    {
        for (uint32_t shift = 0; shift < 64; shift += 8)
            U8(static_cast<uint8_t>(value >> shift));
    }

    void Float(float value) { U32(std::bit_cast<uint32_t>(value)); }

    void Bytes(std::span<const std::byte> bytes)
    {
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
    }

    void Bytes(std::span<const uint8_t> bytes)
    {
        for (uint8_t value : bytes)
            U8(value);
    }

    void String(const std::string& value)
    {
        U64(value.size());
        Bytes(std::as_bytes(std::span(value.data(), value.size())));
    }

    void PadTo(uint64_t alignment)
    {
        const uint64_t aligned = AlignUp(m_bytes.size(), alignment);
        m_bytes.resize(static_cast<size_t>(aligned), std::byte{ 0 });
    }

    std::vector<std::byte>& Data() { return m_bytes; }
    const std::vector<std::byte>& Data() const { return m_bytes; }

private:
    std::vector<std::byte> m_bytes;
};

class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

    bool U8(uint8_t& value)
    {
        if (m_offset >= m_bytes.size())
            return false;
        value = std::to_integer<uint8_t>(m_bytes[m_offset++]);
        return true;
    }

    bool U16(uint16_t& value)
    {
        uint8_t a = 0;
        uint8_t b = 0;
        if (!U8(a) || !U8(b))
            return false;
        value = static_cast<uint16_t>(a | (static_cast<uint16_t>(b) << 8));
        return true;
    }

    bool U32(uint32_t& value)
    {
        value = 0;
        for (uint32_t shift = 0; shift < 32; shift += 8)
        {
            uint8_t byte = 0;
            if (!U8(byte))
                return false;
            value |= static_cast<uint32_t>(byte) << shift;
        }
        return true;
    }

    bool U64(uint64_t& value)
    {
        value = 0;
        for (uint32_t shift = 0; shift < 64; shift += 8)
        {
            uint8_t byte = 0;
            if (!U8(byte))
                return false;
            value |= static_cast<uint64_t>(byte) << shift;
        }
        return true;
    }

    bool Float(float& value)
    {
        uint32_t bits = 0;
        if (!U32(bits))
            return false;
        value = std::bit_cast<float>(bits);
        return true;
    }

    bool Bytes(std::span<std::byte> output)
    {
        if (output.size() > Remaining())
            return false;
        std::memcpy(output.data(), m_bytes.data() + m_offset, output.size());
        m_offset += output.size();
        return true;
    }

    bool Bytes(std::span<uint8_t> output)
    {
        return Bytes(std::as_writable_bytes(output));
    }

    bool String(std::string& value, uint64_t limit)
    {
        uint64_t size = 0;
        if (!U64(size) || size > limit || size > Remaining() ||
            size > (std::numeric_limits<size_t>::max)())
        {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(m_bytes.data() + m_offset),
                     static_cast<size_t>(size));
        m_offset += static_cast<size_t>(size);
        return true;
    }

    size_t Offset() const { return m_offset; }
    size_t Remaining() const { return m_bytes.size() - m_offset; }
    bool Finished() const { return m_offset == m_bytes.size(); }

private:
    std::span<const std::byte> m_bytes;
    size_t m_offset = 0;
};

uint32_t ComputeCookedFileCrc32(std::span<const std::byte> bytes)
{
    constexpr size_t crcOffset = 24;
    constexpr size_t crcBytes = 4;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        const uint8_t value = i >= crcOffset && i < crcOffset + crcBytes
            ? 0
            : std::to_integer<uint8_t>(bytes[i]);
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void WriteVec2(Writer& writer, const core::Vec2f& value)
{
    writer.Float(value.x);
    writer.Float(value.y);
}

void WriteVec3(Writer& writer, const core::Vec3f& value)
{
    writer.Float(value.x);
    writer.Float(value.y);
    writer.Float(value.z);
}

void WriteVec4(Writer& writer, const core::Vec4f& value)
{
    writer.Float(value.x);
    writer.Float(value.y);
    writer.Float(value.z);
    writer.Float(value.w);
}

void WriteColor(Writer& writer, const core::Color& value)
{
    writer.Float(value.r);
    writer.Float(value.g);
    writer.Float(value.b);
    writer.Float(value.a);
}

void WriteBounds(Writer& writer, const Bounds3f& bounds)
{
    writer.U8(bounds.valid ? 1 : 0);
    WriteVec3(writer, bounds.min);
    WriteVec3(writer, bounds.max);
}

void WriteBinding(Writer& writer, const TextureBinding& binding)
{
    writer.U32(binding.textureIndex);
    writer.U32(binding.texCoord);
    writer.Float(binding.strength);
    WriteVec2(writer, binding.offset);
    WriteVec2(writer, binding.scale);
    writer.Float(binding.rotation);
}

bool ReadVec2(Reader& reader, core::Vec2f& value)
{
    return reader.Float(value.x) && reader.Float(value.y) && IsFinite(value);
}

bool ReadVec3(Reader& reader, core::Vec3f& value)
{
    return reader.Float(value.x) && reader.Float(value.y) && reader.Float(value.z) &&
           IsFinite(value);
}

bool ReadVec4(Reader& reader, core::Vec4f& value)
{
    return reader.Float(value.x) && reader.Float(value.y) && reader.Float(value.z) &&
           reader.Float(value.w) && IsFinite(value);
}

bool ReadColor(Reader& reader, core::Color& value)
{
    return reader.Float(value.r) && reader.Float(value.g) && reader.Float(value.b) &&
           reader.Float(value.a) && IsFinite(value);
}

bool ReadBounds(Reader& reader, Bounds3f& bounds)
{
    uint8_t valid = 0;
    if (!reader.U8(valid) || valid > 1 || !ReadVec3(reader, bounds.min) ||
        !ReadVec3(reader, bounds.max))
    {
        return false;
    }
    bounds.valid = valid != 0;
    if (bounds.valid && (bounds.min.x > bounds.max.x || bounds.min.y > bounds.max.y ||
                         bounds.min.z > bounds.max.z))
    {
        return false;
    }
    return true;
}

bool ReadBinding(Reader& reader, TextureBinding& binding)
{
    return reader.U32(binding.textureIndex) && reader.U32(binding.texCoord) &&
           reader.Float(binding.strength) && ReadVec2(reader, binding.offset) &&
           ReadVec2(reader, binding.scale) && reader.Float(binding.rotation) &&
           IsFinite(binding.strength) && IsFinite(binding.rotation) && binding.texCoord <= 1;
}

bool ValidateBinding(const TextureBinding& binding, size_t textureCount)
{
    return (!binding.IsValid() || binding.textureIndex < textureCount) &&
           binding.texCoord <= 1 && IsFinite(binding.strength) &&
           IsFinite(binding.offset) && IsFinite(binding.scale) && IsFinite(binding.rotation);
}

bool ValidateModel(const ImportedModel& model, std::string& error)
{
    if (!IsOrderedFinite(model.bounds))
    {
        error = "model has invalid bounds";
        return false;
    }

    for (const ImportedPrimitive& primitive : model.primitives)
    {
        if (primitive.vertices.empty() || primitive.indices.empty() ||
            primitive.indices.size() % 3 != 0 || !IsOrderedFinite(primitive.bounds) ||
            (primitive.materialIndex != kInvalidAssetIndex &&
             primitive.materialIndex >= model.materials.size()))
        {
            error = "model contains an invalid primitive";
            return false;
        }
        for (uint32_t index : primitive.indices)
        {
            if (index >= primitive.vertices.size())
            {
                error = "primitive index references a missing vertex";
                return false;
            }
        }
        for (const ImportedVertex& vertex : primitive.vertices)
        {
            if (!IsFinite(vertex.position) || !IsFinite(vertex.normal) ||
                !IsFinite(vertex.color) || !IsFinite(vertex.uv0) || !IsFinite(vertex.uv1) ||
                !IsFinite(vertex.tangent) || (vertex.texcoordMask & ~3u) != 0)
            {
                error = "primitive contains an invalid vertex";
                return false;
            }
        }
    }

    for (const ImportedMaterial& material : model.materials)
    {
        const TextureBinding* bindings[] = {
            &material.baseColorTexture, &material.normalTexture,
            &material.metallicRoughnessTexture, &material.occlusionTexture,
            &material.emissiveTexture
        };
        if (!IsFinite(material.baseColor) || !IsFinite(material.emissive) ||
            !IsUnit(material.baseColor) || !IsUnit(material.emissive) ||
            !IsUnit(material.roughness) || !IsUnit(material.metallic) ||
            !IsFinite(material.emissiveStrength) || material.emissiveStrength < 0.0f ||
            !IsUnit(material.alphaCutoff) ||
            static_cast<uint8_t>(material.alphaMode) > static_cast<uint8_t>(AlphaMode::Blend))
        {
            error = "model contains an invalid material";
            return false;
        }
        for (const TextureBinding* binding : bindings)
        {
            if (!ValidateBinding(*binding, model.textures.size()))
            {
                error = "material contains an invalid texture binding";
                return false;
            }
        }
    }

    for (const ImportedTexture& texture : model.textures)
    {
        if ((texture.imageIndex != kInvalidAssetIndex && texture.imageIndex >= model.images.size()) ||
            (texture.samplerIndex != kInvalidAssetIndex &&
             texture.samplerIndex >= model.samplers.size()))
        {
            error = "model contains an invalid texture reference";
            return false;
        }
    }
    for (const ImportedSampler& sampler : model.samplers)
    {
        if (!IsValidTextureFilter(sampler.minFilter) ||
            !IsValidTextureFilter(sampler.magFilter) || !IsValidTextureWrap(sampler.wrapU) ||
            !IsValidTextureWrap(sampler.wrapV))
        {
            error = "model contains an invalid sampler";
            return false;
        }
    }
    return true;
}

bool FitsStringLimit(const std::string& value, const CookedModelLimits& limits)
{
    return value.size() <= limits.maxStringBytes;
}

bool ValidateBuildLimits(
    const ImportedModel& model,
    const std::vector<CookedDependency>& dependencies,
    const CookedModelLimits& limits,
    std::string& error)
{
    if (model.primitives.size() > limits.maxPrimitives ||
        model.VertexCount() > limits.maxVertices || model.IndexCount() > limits.maxIndices ||
        model.materials.size() > limits.maxMaterials || model.images.size() > limits.maxImages ||
        model.samplers.size() > limits.maxSamplers || model.textures.size() > limits.maxTextures ||
        dependencies.size() > limits.maxDependencies)
    {
        error = "model exceeds configured cooked-format table limits";
        return false;
    }
    if (!FitsStringLimit(model.name, limits))
    {
        error = "model name exceeds configured string limit";
        return false;
    }
    for (const ImportedPrimitive& primitive : model.primitives)
    {
        if (!FitsStringLimit(primitive.name, limits) ||
            !FitsStringLimit(primitive.nodeName, limits))
        {
            error = "primitive name exceeds configured string limit";
            return false;
        }
    }
    for (const ImportedMaterial& material : model.materials)
    {
        if (!FitsStringLimit(material.name, limits))
        {
            error = "material name exceeds configured string limit";
            return false;
        }
    }
    uint64_t totalImageBytes = 0;
    for (const ImageSource& image : model.images)
    {
        if (!FitsStringLimit(image.name, limits) || !FitsStringLimit(image.uri, limits) ||
            !FitsStringLimit(image.mimeType, limits) ||
            !CheckedAdd(totalImageBytes, image.embeddedBytes.size(), totalImageBytes) ||
            totalImageBytes > limits.maxEmbeddedImageBytes)
        {
            error = "image data exceeds configured cooked-format limits";
            return false;
        }
    }
    for (const ImportedSampler& sampler : model.samplers)
    {
        if (!FitsStringLimit(sampler.name, limits))
        {
            error = "sampler name exceeds configured string limit";
            return false;
        }
    }
    for (const ImportedTexture& texture : model.textures)
    {
        if (!FitsStringLimit(texture.name, limits))
        {
            error = "texture name exceeds configured string limit";
            return false;
        }
    }
    for (const CookedDependency& dependency : dependencies)
    {
        if (!FitsStringLimit(dependency.uri, limits))
        {
            error = "dependency URI exceeds configured string limit";
            return false;
        }
    }

    std::array<uint64_t, kSectionCount> sectionSizes = { 8 + 25 + 8, 8, 8, 8, 8, 8 };
    const auto add = [&error](uint64_t& target, uint64_t value) {
        if (!CheckedAdd(target, value, target))
        {
            error = "cooked section size overflow";
            return false;
        }
        return true;
    };
    if (!add(sectionSizes[0], model.name.size()))
        return false;
    for (const CookedDependency& dependency : dependencies)
    {
        if (!add(sectionSizes[0], 8 + dependency.uri.size() + 8 + 32))
            return false;
    }
    for (const ImportedPrimitive& primitive : model.primitives)
    {
        uint64_t vertexBytes = 0;
        uint64_t indexBytes = 0;
        if (!CheckedMultiply(primitive.vertices.size(), 73, vertexBytes) ||
            !CheckedMultiply(primitive.indices.size(), sizeof(uint32_t), indexBytes) ||
            !add(sectionSizes[1], 8 + primitive.name.size() + 8 +
                 primitive.nodeName.size() + 4 + 25 + 8) ||
            !add(sectionSizes[1], vertexBytes) || !add(sectionSizes[1], 8) ||
            !add(sectionSizes[1], indexBytes))
        {
            error = "cooked geometry section size overflow";
            return false;
        }
    }
    for (const ImportedMaterial& material : model.materials)
    {
        if (!add(sectionSizes[2], 8 + material.name.size() + 211))
            return false;
    }
    for (const ImageSource& image : model.images)
    {
        if (!add(sectionSizes[3], 32 + image.name.size() + image.uri.size() +
                 image.mimeType.size() + image.embeddedBytes.size()))
        {
            return false;
        }
    }
    for (const ImportedSampler& sampler : model.samplers)
    {
        if (!add(sectionSizes[4], 16 + sampler.name.size()))
            return false;
    }
    for (const ImportedTexture& texture : model.textures)
    {
        if (!add(sectionSizes[5], 16 + texture.name.size()))
            return false;
    }
    uint64_t estimatedFileBytes = AlignUp(
        kFixedHeaderBytes + kSectionCount * kSectionEntryBytes, 8);
    for (uint64_t sectionSize : sectionSizes)
    {
        if (sectionSize > limits.maxSectionBytes)
        {
            error = "cooked section exceeds configured size limit";
            return false;
        }
        estimatedFileBytes = AlignUp(estimatedFileBytes, 8);
        if (estimatedFileBytes == 0 ||
            !CheckedAdd(estimatedFileBytes, sectionSize, estimatedFileBytes))
        {
            error = "cooked file size overflow";
            return false;
        }
    }
    if (estimatedFileBytes > limits.maxFileBytes)
    {
        error = "cooked file exceeds configured size limit";
        return false;
    }
    return true;
}

Section BuildMetadataSection(
    const ImportedModel& model,
    const std::vector<CookedDependency>& dependencies)
{
    Writer writer;
    writer.String(model.name);
    WriteBounds(writer, model.bounds);
    writer.U64(dependencies.size());
    for (const CookedDependency& dependency : dependencies)
    {
        writer.String(dependency.uri);
        writer.U64(dependency.byteSize);
        writer.Bytes(dependency.sha256.bytes);
    }
    return { SectionType::Metadata, dependencies.size(), std::move(writer.Data()), 0 };
}

Section BuildGeometrySection(const ImportedModel& model)
{
    Writer writer;
    writer.U64(model.primitives.size());
    for (const ImportedPrimitive& primitive : model.primitives)
    {
        writer.String(primitive.name);
        writer.String(primitive.nodeName);
        writer.U32(primitive.materialIndex);
        WriteBounds(writer, primitive.bounds);
        writer.U64(primitive.vertices.size());
        for (const ImportedVertex& vertex : primitive.vertices)
        {
            WriteVec3(writer, vertex.position);
            WriteVec3(writer, vertex.normal);
            WriteColor(writer, vertex.color);
            WriteVec2(writer, vertex.uv0);
            WriteVec2(writer, vertex.uv1);
            writer.U8(vertex.texcoordMask);
            WriteVec4(writer, vertex.tangent);
        }
        writer.U64(primitive.indices.size());
        for (uint32_t index : primitive.indices)
            writer.U32(index);
    }
    return { SectionType::Geometry, model.primitives.size(), std::move(writer.Data()), 0 };
}

Section BuildMaterialSection(const ImportedModel& model)
{
    Writer writer;
    writer.U64(model.materials.size());
    for (const ImportedMaterial& material : model.materials)
    {
        writer.String(material.name);
        WriteColor(writer, material.baseColor);
        writer.Float(material.roughness);
        writer.Float(material.metallic);
        WriteColor(writer, material.emissive);
        writer.Float(material.emissiveStrength);
        writer.Float(material.alphaCutoff);
        writer.U8(static_cast<uint8_t>(material.alphaMode));
        writer.U8(material.doubleSided ? 1 : 0);
        writer.U8(material.unlit ? 1 : 0);
        WriteBinding(writer, material.baseColorTexture);
        WriteBinding(writer, material.normalTexture);
        WriteBinding(writer, material.metallicRoughnessTexture);
        WriteBinding(writer, material.occlusionTexture);
        WriteBinding(writer, material.emissiveTexture);
    }
    return { SectionType::Materials, model.materials.size(), std::move(writer.Data()), 0 };
}

Section BuildImageSection(const ImportedModel& model)
{
    Writer writer;
    writer.U64(model.images.size());
    for (const ImageSource& image : model.images)
    {
        writer.String(image.name);
        writer.String(image.uri);
        writer.String(image.mimeType);
        writer.U64(image.embeddedBytes.size());
        writer.Bytes(image.embeddedBytes);
    }
    return { SectionType::Images, model.images.size(), std::move(writer.Data()), 0 };
}

Section BuildSamplerSection(const ImportedModel& model)
{
    Writer writer;
    writer.U64(model.samplers.size());
    for (const ImportedSampler& sampler : model.samplers)
    {
        writer.String(sampler.name);
        writer.U16(static_cast<uint16_t>(sampler.minFilter));
        writer.U16(static_cast<uint16_t>(sampler.magFilter));
        writer.U16(static_cast<uint16_t>(sampler.wrapU));
        writer.U16(static_cast<uint16_t>(sampler.wrapV));
    }
    return { SectionType::Samplers, model.samplers.size(), std::move(writer.Data()), 0 };
}

Section BuildTextureSection(const ImportedModel& model)
{
    Writer writer;
    writer.U64(model.textures.size());
    for (const ImportedTexture& texture : model.textures)
    {
        writer.String(texture.name);
        writer.U32(texture.imageIndex);
        writer.U32(texture.samplerIndex);
    }
    return { SectionType::Textures, model.textures.size(), std::move(writer.Data()), 0 };
}

CookedModelResult LoadFailure(CookedModelStatus status, std::string error)
{
    CookedModelResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

bool ReadCount(Reader& reader, uint64_t limit, size_t& count, bool& limitExceeded)
{
    uint64_t value = 0;
    if (!reader.U64(value))
        return false;
    if (value > limit || value > (std::numeric_limits<size_t>::max)())
    {
        limitExceeded = true;
        return false;
    }
    count = static_cast<size_t>(value);
    return true;
}

bool DecodeMetadata(
    const SectionView& section,
    ImportedModel& model,
    CookedModelMetadata& metadata,
    const CookedModelLimits& limits,
    bool& limitExceeded)
{
    Reader reader(section.bytes);
    if (!reader.String(model.name, limits.maxStringBytes) || !ReadBounds(reader, model.bounds))
        return false;
    size_t count = 0;
    if (!ReadCount(reader, limits.maxDependencies, count, limitExceeded) ||
        count != section.elementCount)
        return false;
    constexpr size_t minimumDependencyBytes = 8 + 8 + 32;
    if (count > reader.Remaining() / minimumDependencyBytes)
        return false;
    metadata.dependencies.resize(count);
    std::string previousUri;
    for (CookedDependency& dependency : metadata.dependencies)
    {
        if (!reader.String(dependency.uri, limits.maxStringBytes) || dependency.uri.empty() ||
            !reader.U64(dependency.byteSize) || !reader.Bytes(dependency.sha256.bytes) ||
            (!previousUri.empty() && dependency.uri <= previousUri))
        {
            return false;
        }
        previousUri = dependency.uri;
    }
    return reader.Finished();
}

bool DecodeGeometry(
    const SectionView& section,
    ImportedModel& model,
    const CookedModelLimits& limits,
    bool& limitExceeded)
{
    Reader reader(section.bytes);
    size_t primitiveCount = 0;
    if (!ReadCount(reader, limits.maxPrimitives, primitiveCount, limitExceeded) ||
        primitiveCount != section.elementCount)
    {
        return false;
    }
    constexpr size_t minimumPrimitiveBytes = 8 + 8 + 4 + 25 + 8 + 8;
    if (primitiveCount > reader.Remaining() / minimumPrimitiveBytes)
        return false;
    model.primitives.resize(primitiveCount);
    uint64_t totalVertices = 0;
    uint64_t totalIndices = 0;
    for (ImportedPrimitive& primitive : model.primitives)
    {
        if (!reader.String(primitive.name, limits.maxStringBytes) ||
            !reader.String(primitive.nodeName, limits.maxStringBytes) ||
            !reader.U32(primitive.materialIndex) || !ReadBounds(reader, primitive.bounds))
        {
            return false;
        }
        size_t vertexCount = 0;
        if (!ReadCount(reader, limits.maxVertices, vertexCount, limitExceeded))
        {
            return false;
        }
        if (!CheckedAdd(totalVertices, vertexCount, totalVertices) ||
            totalVertices > limits.maxVertices)
        {
            limitExceeded = true;
            return false;
        }
        constexpr size_t serializedVertexBytes = 73;
        if (vertexCount > reader.Remaining() / serializedVertexBytes)
            return false;
        primitive.vertices.resize(vertexCount);
        for (ImportedVertex& vertex : primitive.vertices)
        {
            if (!ReadVec3(reader, vertex.position) || !ReadVec3(reader, vertex.normal) ||
                !ReadColor(reader, vertex.color) || !ReadVec2(reader, vertex.uv0) ||
                !ReadVec2(reader, vertex.uv1) || !reader.U8(vertex.texcoordMask) ||
                (vertex.texcoordMask & ~3u) != 0 || !ReadVec4(reader, vertex.tangent))
            {
                return false;
            }
        }
        size_t indexCount = 0;
        if (!ReadCount(reader, limits.maxIndices, indexCount, limitExceeded))
        {
            return false;
        }
        if (!CheckedAdd(totalIndices, indexCount, totalIndices) ||
            totalIndices > limits.maxIndices)
        {
            limitExceeded = true;
            return false;
        }
        if (indexCount > reader.Remaining() / sizeof(uint32_t))
            return false;
        primitive.indices.resize(indexCount);
        for (uint32_t& index : primitive.indices)
        {
            if (!reader.U32(index) || index >= primitive.vertices.size())
                return false;
        }
    }
    return reader.Finished();
}

bool DecodeMaterials(
    const SectionView& section,
    ImportedModel& model,
    const CookedModelLimits& limits,
    bool& limitExceeded)
{
    Reader reader(section.bytes);
    size_t count = 0;
    if (!ReadCount(reader, limits.maxMaterials, count, limitExceeded) ||
        count != section.elementCount)
        return false;
    constexpr size_t minimumMaterialBytes = 8 + 16 + 4 + 4 + 16 + 4 + 4 + 3 + 5 * 32;
    if (count > reader.Remaining() / minimumMaterialBytes)
        return false;
    model.materials.resize(count);
    for (ImportedMaterial& material : model.materials)
    {
        uint8_t alphaMode = 0;
        uint8_t doubleSided = 0;
        uint8_t unlit = 0;
        if (!reader.String(material.name, limits.maxStringBytes) ||
            !ReadColor(reader, material.baseColor) || !reader.Float(material.roughness) ||
            !reader.Float(material.metallic) || !ReadColor(reader, material.emissive) ||
            !reader.Float(material.emissiveStrength) || !reader.Float(material.alphaCutoff) ||
            !reader.U8(alphaMode) || alphaMode > static_cast<uint8_t>(AlphaMode::Blend) ||
            !reader.U8(doubleSided) || doubleSided > 1 || !reader.U8(unlit) || unlit > 1 ||
            !ReadBinding(reader, material.baseColorTexture) ||
            !ReadBinding(reader, material.normalTexture) ||
            !ReadBinding(reader, material.metallicRoughnessTexture) ||
            !ReadBinding(reader, material.occlusionTexture) ||
            !ReadBinding(reader, material.emissiveTexture) ||
            !IsFinite(material.roughness) || !IsFinite(material.metallic) ||
            !IsFinite(material.emissiveStrength) || !IsFinite(material.alphaCutoff))
        {
            return false;
        }
        material.alphaMode = static_cast<AlphaMode>(alphaMode);
        material.doubleSided = doubleSided != 0;
        material.unlit = unlit != 0;
    }
    return reader.Finished();
}

bool DecodeImages(
    const SectionView& section,
    ImportedModel& model,
    const CookedModelLimits& limits,
    bool& limitExceeded)
{
    Reader reader(section.bytes);
    size_t count = 0;
    if (!ReadCount(reader, limits.maxImages, count, limitExceeded) ||
        count != section.elementCount)
        return false;
    constexpr size_t minimumImageBytes = 8 + 8 + 8 + 8;
    if (count > reader.Remaining() / minimumImageBytes)
        return false;
    model.images.resize(count);
    uint64_t totalBytes = 0;
    for (ImageSource& image : model.images)
    {
        uint64_t byteCount = 0;
        if (!reader.String(image.name, limits.maxStringBytes) ||
            !reader.String(image.uri, limits.maxStringBytes) ||
            !reader.String(image.mimeType, limits.maxStringBytes) || !reader.U64(byteCount))
        {
            return false;
        }
        if (!CheckedAdd(totalBytes, byteCount, totalBytes) ||
            totalBytes > limits.maxEmbeddedImageBytes)
        {
            limitExceeded = true;
            return false;
        }
        if (byteCount > reader.Remaining() ||
            byteCount > (std::numeric_limits<size_t>::max)())
        {
            return false;
        }
        image.embeddedBytes.resize(static_cast<size_t>(byteCount));
        if (!reader.Bytes(image.embeddedBytes))
            return false;
    }
    return reader.Finished();
}

bool DecodeSamplers(
    const SectionView& section,
    ImportedModel& model,
    const CookedModelLimits& limits,
    bool& limitExceeded)
{
    Reader reader(section.bytes);
    size_t count = 0;
    if (!ReadCount(reader, limits.maxSamplers, count, limitExceeded) ||
        count != section.elementCount)
        return false;
    constexpr size_t minimumSamplerBytes = 8 + 4 * sizeof(uint16_t);
    if (count > reader.Remaining() / minimumSamplerBytes)
        return false;
    model.samplers.resize(count);
    for (ImportedSampler& sampler : model.samplers)
    {
        uint16_t minFilter = 0;
        uint16_t magFilter = 0;
        uint16_t wrapU = 0;
        uint16_t wrapV = 0;
        if (!reader.String(sampler.name, limits.maxStringBytes) || !reader.U16(minFilter) ||
            !reader.U16(magFilter) || !reader.U16(wrapU) || !reader.U16(wrapV))
        {
            return false;
        }
        sampler.minFilter = static_cast<TextureFilter>(minFilter);
        sampler.magFilter = static_cast<TextureFilter>(magFilter);
        sampler.wrapU = static_cast<TextureWrap>(wrapU);
        sampler.wrapV = static_cast<TextureWrap>(wrapV);
        if (!IsValidTextureFilter(sampler.minFilter) ||
            !IsValidTextureFilter(sampler.magFilter) || !IsValidTextureWrap(sampler.wrapU) ||
            !IsValidTextureWrap(sampler.wrapV))
        {
            return false;
        }
    }
    return reader.Finished();
}

bool DecodeTextures(
    const SectionView& section,
    ImportedModel& model,
    const CookedModelLimits& limits,
    bool& limitExceeded)
{
    Reader reader(section.bytes);
    size_t count = 0;
    if (!ReadCount(reader, limits.maxTextures, count, limitExceeded) ||
        count != section.elementCount)
        return false;
    constexpr size_t minimumTextureBytes = 8 + 2 * sizeof(uint32_t);
    if (count > reader.Remaining() / minimumTextureBytes)
        return false;
    model.textures.resize(count);
    for (ImportedTexture& texture : model.textures)
    {
        if (!reader.String(texture.name, limits.maxStringBytes) ||
            !reader.U32(texture.imageIndex) || !reader.U32(texture.samplerIndex))
        {
            return false;
        }
    }
    return reader.Finished();
}

void ShaTransform(uint32_t state[8], const uint8_t block[64])
{
    static constexpr uint32_t constants[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t words[64] = {};
    for (int i = 0; i < 16; ++i)
    {
        words[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i)
    {
        const uint32_t s0 = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^
                            (words[i - 15] >> 3);
        const uint32_t s1 = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^
                            (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = state[0]; uint32_t b = state[1]; uint32_t c = state[2]; uint32_t d = state[3];
    uint32_t e = state[4]; uint32_t f = state[5]; uint32_t g = state[6]; uint32_t h = state[7];
    for (int i = 0; i < 64; ++i)
    {
        const uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + s1 + choice + constants[i] + words[i];
        const uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + majority;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

} // namespace

std::string Sha256Digest::Hex() const
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(bytes.size() * 2);
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        result[i * 2 + 0] = digits[bytes[i] >> 4];
        result[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return result;
}

Sha256Digest ComputeSha256(std::span<const std::byte> bytes)
{
    uint32_t state[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    size_t offset = 0;
    while (bytes.size() - offset >= 64)
    {
        uint8_t block[64];
        for (size_t i = 0; i < 64; ++i)
            block[i] = std::to_integer<uint8_t>(bytes[offset + i]);
        ShaTransform(state, block);
        offset += 64;
    }

    uint8_t tail[128] = {};
    const size_t remainder = bytes.size() - offset;
    for (size_t i = 0; i < remainder; ++i)
        tail[i] = std::to_integer<uint8_t>(bytes[offset + i]);
    tail[remainder] = 0x80;
    const size_t tailBytes = remainder < 56 ? 64 : 128;
    const uint64_t bitLength = static_cast<uint64_t>(bytes.size()) * 8ull;
    for (int i = 0; i < 8; ++i)
        tail[tailBytes - 1 - i] = static_cast<uint8_t>(bitLength >> (i * 8));
    ShaTransform(state, tail);
    if (tailBytes == 128)
        ShaTransform(state, tail + 64);

    Sha256Digest digest;
    for (size_t i = 0; i < 8; ++i)
    {
        digest.bytes[i * 4 + 0] = static_cast<uint8_t>(state[i] >> 24);
        digest.bytes[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
        digest.bytes[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
        digest.bytes[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
    return digest;
}

CookedBuildResult BuildCookedModel(
    const ImportedModel& model,
    const CookedModelMetadata& metadata,
    const CookedModelLimits& limits)
{
    CookedBuildResult result;
    try
    {
        if (!ValidateModel(model, result.error))
            return result;

        std::vector<CookedDependency> dependencies = metadata.dependencies;
        std::sort(dependencies.begin(), dependencies.end(),
                  [](const CookedDependency& left, const CookedDependency& right) {
                      return left.uri < right.uri;
                  });
        for (size_t i = 0; i < dependencies.size(); ++i)
        {
            if (dependencies[i].uri.empty() ||
                (i > 0 && dependencies[i - 1].uri == dependencies[i].uri))
            {
                result.error = "dependency URIs must be nonempty and unique";
                return result;
            }
        }
        if (!ValidateBuildLimits(model, dependencies, limits, result.error))
        {
            result.status = CookedModelStatus::ResourceLimitExceeded;
            return result;
        }

        std::array<Section, kSectionCount> sections = {
            BuildMetadataSection(model, dependencies),
            BuildGeometrySection(model),
            BuildMaterialSection(model),
            BuildImageSection(model),
            BuildSamplerSection(model),
            BuildTextureSection(model)
        };

        const uint64_t headerBytes = AlignUp(
            kFixedHeaderBytes + kSectionCount * kSectionEntryBytes, 8);
        uint64_t fileSize = headerBytes;
        for (Section& section : sections)
        {
            if (section.bytes.size() > limits.maxSectionBytes)
            {
                result.status = CookedModelStatus::ResourceLimitExceeded;
                result.error = "cooked section exceeds configured size limit";
                return result;
            }
            fileSize = AlignUp(fileSize, 8);
            if (fileSize == 0)
            {
                result.error = "cooked file size overflow";
                return result;
            }
            section.offset = fileSize;
            if (!CheckedAdd(fileSize, section.bytes.size(), fileSize))
            {
                result.error = "cooked file size overflow";
                return result;
            }
        }
        if (fileSize > limits.maxFileBytes ||
            fileSize > (std::numeric_limits<size_t>::max)())
        {
            result.status = CookedModelStatus::ResourceLimitExceeded;
            result.error = "cooked file is too large for this process";
            return result;
        }

        Writer writer;
        writer.Bytes(kMagic);
        writer.U32(kFormatVersion);
        writer.U32(static_cast<uint32_t>(headerBytes));
        writer.U64(fileSize);
        writer.U32(0);
        writer.U32(kSectionCount);
        writer.Bytes(metadata.sourceSha256.bytes);
        writer.U64(0);
        for (const Section& section : sections)
        {
            writer.U32(static_cast<uint32_t>(section.type));
            writer.U32(kSectionVersion);
            writer.U64(section.offset);
            writer.U64(section.bytes.size());
            writer.U64(section.elementCount);
        }
        writer.PadTo(8);
        for (const Section& section : sections)
        {
            writer.PadTo(8);
            writer.Bytes(section.bytes);
        }
        if (writer.Data().size() != fileSize)
        {
            result.error = "internal cooked layout mismatch";
            return result;
        }

        const uint32_t crc = ComputeCookedFileCrc32(writer.Data());
        for (int i = 0; i < 4; ++i)
            writer.Data()[24 + i] = static_cast<std::byte>(crc >> (i * 8));

        result.status = CookedModelStatus::Success;
        result.bytes = std::move(writer.Data());
        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.status = CookedModelStatus::ResourceLimitExceeded;
        result.error = "out of memory while building cooked model";
        return result;
    }
}

CookedModelResult LoadCookedModelMemory(
    std::span<const std::byte> bytes,
    const CookedModelLimits& limits)
{
    try
    {
        if (bytes.size() > limits.maxFileBytes)
            return LoadFailure(CookedModelStatus::ResourceLimitExceeded, "cooked file exceeds size limit");
        if (bytes.size() < kFixedHeaderBytes)
            return LoadFailure(CookedModelStatus::InvalidLayout, "cooked file is truncated");

        Reader reader(bytes);
        std::array<uint8_t, 8> magic{};
        if (!reader.Bytes(magic) || magic != kMagic)
            return LoadFailure(CookedModelStatus::InvalidMagic, "cooked model magic is invalid");

        uint32_t version = 0;
        uint32_t headerBytes = 0;
        uint64_t fileSize = 0;
        uint32_t payloadCrc = 0;
        uint32_t sectionCount = 0;
        CookedModelResult result;
        uint64_t reserved = 0;
        if (!reader.U32(version))
            return LoadFailure(CookedModelStatus::InvalidLayout, "cooked header is truncated");
        if (version != kFormatVersion)
            return LoadFailure(CookedModelStatus::UnsupportedVersion, "cooked model version is unsupported");
        if (!reader.U32(headerBytes) || !reader.U64(fileSize) || !reader.U32(payloadCrc) ||
            !reader.U32(sectionCount) || !reader.Bytes(result.metadata.sourceSha256.bytes) ||
            !reader.U64(reserved))
        {
            return LoadFailure(CookedModelStatus::InvalidLayout, "cooked header is truncated");
        }
        const uint64_t expectedHeaderBytes = AlignUp(
            kFixedHeaderBytes + static_cast<uint64_t>(sectionCount) * kSectionEntryBytes, 8);
        if (sectionCount != kSectionCount || expectedHeaderBytes != headerBytes ||
            reserved != 0 ||
            fileSize != bytes.size() || headerBytes > bytes.size())
        {
            return LoadFailure(CookedModelStatus::InvalidLayout, "cooked header layout is invalid");
        }

        std::vector<SectionView> sections;
        sections.reserve(sectionCount);
        std::unordered_set<uint32_t> seenTypes;
        for (uint32_t i = 0; i < sectionCount; ++i)
        {
            uint32_t rawType = 0;
            uint32_t sectionVersion = 0;
            uint64_t offset = 0;
            uint64_t size = 0;
            uint64_t count = 0;
            if (!reader.U32(rawType) || !reader.U32(sectionVersion) || !reader.U64(offset) ||
                !reader.U64(size) || !reader.U64(count) || sectionVersion != kSectionVersion ||
                rawType < static_cast<uint32_t>(SectionType::Metadata) ||
                rawType > static_cast<uint32_t>(SectionType::Textures) ||
                !seenTypes.insert(rawType).second || offset < headerBytes || offset % 8 != 0 ||
                size > limits.maxSectionBytes || offset > bytes.size() || size > bytes.size() - offset)
            {
                return LoadFailure(CookedModelStatus::InvalidLayout, "cooked section table is invalid");
            }
            sections.push_back({ static_cast<SectionType>(rawType), count,
                                 bytes.subspan(static_cast<size_t>(offset), static_cast<size_t>(size)),
                                 offset });
        }
        std::vector<SectionView> byOffset = sections;
        std::sort(byOffset.begin(), byOffset.end(),
                  [](const SectionView& left, const SectionView& right) {
                      return left.offset < right.offset;
                  });
        uint64_t previousEnd = headerBytes;
        for (const SectionView& section : byOffset)
        {
            if (section.offset < previousEnd)
                return LoadFailure(CookedModelStatus::InvalidLayout, "cooked sections overlap");
            previousEnd = section.offset + section.bytes.size();
        }
        if (ComputeCookedFileCrc32(bytes) != payloadCrc)
            return LoadFailure(CookedModelStatus::IntegrityMismatch, "cooked file CRC32 does not match");

        const auto findSection = [&sections](SectionType type) -> const SectionView& {
            return *std::find_if(sections.begin(), sections.end(),
                                 [type](const SectionView& section) { return section.type == type; });
        };
        bool limitExceeded = false;
        if (!DecodeMetadata(findSection(SectionType::Metadata), result.model, result.metadata,
                            limits, limitExceeded) ||
            !DecodeGeometry(findSection(SectionType::Geometry), result.model, limits,
                            limitExceeded) ||
            !DecodeMaterials(findSection(SectionType::Materials), result.model, limits,
                             limitExceeded) ||
            !DecodeImages(findSection(SectionType::Images), result.model, limits,
                          limitExceeded) ||
            !DecodeSamplers(findSection(SectionType::Samplers), result.model, limits,
                            limitExceeded) ||
            !DecodeTextures(findSection(SectionType::Textures), result.model, limits,
                            limitExceeded))
        {
            if (limitExceeded)
                return LoadFailure(CookedModelStatus::ResourceLimitExceeded,
                                   "cooked section exceeds configured resource limits");
            return LoadFailure(CookedModelStatus::InvalidData, "cooked section data is invalid");
        }

        std::string validationError;
        if (!ValidateModel(result.model, validationError))
            return LoadFailure(CookedModelStatus::InvalidData, std::move(validationError));
        result.status = CookedModelStatus::Success;
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return LoadFailure(CookedModelStatus::ResourceLimitExceeded,
                           "out of memory while loading cooked model");
    }
}

CookedModelResult LoadCookedModelFile(
    const std::filesystem::path& path,
    const CookedModelLimits& limits)
{
    try
    {
        std::error_code errorCode;
        const bool exists = std::filesystem::exists(path, errorCode);
        if (errorCode)
            return LoadFailure(CookedModelStatus::IoError, "could not inspect cooked model file");
        if (!exists)
            return LoadFailure(CookedModelStatus::FileNotFound, "cooked model file does not exist");
        const uint64_t size = std::filesystem::file_size(path, errorCode);
        if (errorCode)
            return LoadFailure(CookedModelStatus::IoError, "could not determine cooked model size");
        if (size > limits.maxFileBytes || size > (std::numeric_limits<size_t>::max)())
            return LoadFailure(CookedModelStatus::ResourceLimitExceeded, "cooked file exceeds size limit");

        std::vector<std::byte> bytes(static_cast<size_t>(size));
        std::ifstream stream(path, std::ios::binary);
        if (!stream || !stream.read(reinterpret_cast<char*>(bytes.data()),
                                    static_cast<std::streamsize>(bytes.size())))
        {
            return LoadFailure(CookedModelStatus::IoError, "could not read cooked model file");
        }
        return LoadCookedModelMemory(bytes, limits);
    }
    catch (const std::bad_alloc&)
    {
        return LoadFailure(CookedModelStatus::ResourceLimitExceeded,
                           "out of memory while reading cooked model file");
    }
}

CookedModelStatus WriteCookedModelFileAtomic(
    const std::filesystem::path& path,
    std::span<const std::byte> bytes,
    std::string& error)
{
    const CookedModelResult memoryVerification = LoadCookedModelMemory(bytes);
    if (!memoryVerification.Succeeded())
    {
        error = "refusing to publish invalid cooked bytes: " + memoryVerification.error;
        return memoryVerification.status;
    }

    std::error_code errorCode;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), errorCode);
    if (errorCode)
    {
        error = "could not create cooked model output directory";
        return CookedModelStatus::IoError;
    }

    static std::atomic<uint64_t> sequence = 0;
    std::filesystem::path temporary = path;
#if defined(_WIN32)
    temporary += ".tmp." + std::to_string(GetCurrentProcessId()) + "." +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
#else
    temporary += ".tmp." +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
#endif
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream || !stream.write(reinterpret_cast<const char*>(bytes.data()),
                                     static_cast<std::streamsize>(bytes.size())))
        {
            stream.close();
            std::filesystem::remove(temporary, errorCode);
            error = "could not write temporary cooked model file";
            return CookedModelStatus::IoError;
        }
        stream.flush();
        if (!stream)
        {
            stream.close();
            std::filesystem::remove(temporary, errorCode);
            error = "could not flush temporary cooked model file";
            return CookedModelStatus::IoError;
        }
    }

    const CookedModelResult diskVerification = LoadCookedModelFile(temporary);
    if (!diskVerification.Succeeded())
    {
        std::filesystem::remove(temporary, errorCode);
        error = "temporary cooked model verification failed: " + diskVerification.error;
        return diskVerification.status;
    }

#if defined(_WIN32)
    bool published = false;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        if (MoveFileExW(temporary.c_str(), path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            published = true;
            break;
        }
        const DWORD moveError = GetLastError();
        if (moveError != ERROR_SHARING_VIOLATION && moveError != ERROR_ACCESS_DENIED)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!published)
    {
        std::filesystem::remove(temporary, errorCode);
        error = "could not atomically replace cooked model file";
        return CookedModelStatus::IoError;
    }
#else
    std::filesystem::rename(temporary, path, errorCode);
    if (errorCode)
    {
        std::filesystem::remove(temporary, errorCode);
        error = "could not atomically replace cooked model file";
        return CookedModelStatus::IoError;
    }
#endif
    error.clear();
    return CookedModelStatus::Success;
}

const char* CookedModelStatusName(CookedModelStatus status)
{
    switch (status)
    {
    case CookedModelStatus::Success: return "success";
    case CookedModelStatus::FileNotFound: return "file not found";
    case CookedModelStatus::IoError: return "I/O error";
    case CookedModelStatus::InvalidMagic: return "invalid magic";
    case CookedModelStatus::UnsupportedVersion: return "unsupported version";
    case CookedModelStatus::InvalidLayout: return "invalid layout";
    case CookedModelStatus::IntegrityMismatch: return "integrity mismatch";
    case CookedModelStatus::ResourceLimitExceeded: return "resource limit exceeded";
    case CookedModelStatus::InvalidData: return "invalid data";
    default: return "unknown";
    }
}

} // namespace asset
