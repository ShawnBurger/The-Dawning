#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "gltf_importer.h"

#define CGLTF_IMPLEMENTATION
#include "../../third_party/cgltf/cgltf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace asset
{
namespace
{

struct CgltfDeleter
{
    void operator()(cgltf_data* data) const { cgltf_free(data); }
};

using CgltfDataPtr = std::unique_ptr<cgltf_data, CgltfDeleter>;

GltfImportResult Failure(GltfImportStatus status, std::string message)
{
    GltfImportResult result;
    result.status = status;
    result.error = std::move(message);
    return result;
}

const char* CgltfResultName(cgltf_result result)
{
    switch (result)
    {
    case cgltf_result_success: return "success";
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "unknown format";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "I/O error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "legacy glTF 1.x";
    default: return "unknown error";
    }
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

bool CheckedAdd(uint64_t left, uint64_t right, uint64_t& result)
{
    if (right > (std::numeric_limits<uint64_t>::max)() - left)
        return false;
    result = left + right;
    return true;
}

bool IsDataUri(const char* uri)
{
    return uri && std::string_view(uri).starts_with("data:");
}

bool IsControlledRelativeUri(const char* uri)
{
    if (!uri || IsDataUri(uri))
        return true;

    std::string decoded(uri);
    cgltf_decode_uri(decoded.data());
    const std::filesystem::path path(decoded);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;

    for (const std::filesystem::path& component : path)
    {
        if (component == "..")
            return false;
    }
    return true;
}

std::string DecodeUri(const char* uri)
{
    std::string decoded(uri ? uri : "");
    cgltf_decode_uri(decoded.data());
    decoded.resize(std::strlen(decoded.c_str()));
    return decoded;
}

void AddSourceDependency(
    const char* uri,
    GltfDependencyKind kind,
    std::vector<GltfSourceDependency>& dependencies)
{
    if (!uri || IsDataUri(uri))
        return;
    dependencies.push_back({ DecodeUri(uri), kind });
}

bool CollectSourceDependencies(
    const cgltf_data& data,
    std::vector<GltfSourceDependency>& dependencies,
    std::string& error)
{
    for (cgltf_size i = 0; i < data.buffers_count; ++i)
    {
        if (!IsControlledRelativeUri(data.buffers[i].uri))
        {
            error = "buffer URI escapes the controlled asset directory";
            return false;
        }
        AddSourceDependency(data.buffers[i].uri, GltfDependencyKind::Buffer, dependencies);
    }
    for (cgltf_size i = 0; i < data.images_count; ++i)
    {
        if (!IsControlledRelativeUri(data.images[i].uri))
        {
            error = "image URI escapes the controlled asset directory";
            return false;
        }
        AddSourceDependency(data.images[i].uri, GltfDependencyKind::Image, dependencies);
    }
    return true;
}

float Determinant3x3(const core::Mat4x4& matrix)
{
    return matrix.m[0][0] * (matrix.m[1][1] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][1])
         - matrix.m[0][1] * (matrix.m[1][0] * matrix.m[2][2] - matrix.m[1][2] * matrix.m[2][0])
         + matrix.m[0][2] * (matrix.m[1][0] * matrix.m[2][1] - matrix.m[1][1] * matrix.m[2][0]);
}

core::Mat4x4 ToEngineMatrix(const cgltf_float sourceColumnMajor[16])
{
    // C_lh = S * C_rh * S, with S reflecting Z. Mat4x4 stores the transpose
    // because the engine uses row-vector semantics.
    constexpr float sign[4] = { 1.0f, 1.0f, -1.0f, 1.0f };
    core::Mat4x4 result;
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
            result.m[row][column] = sourceColumnMajor[row * 4 + column]
                                  * sign[row] * sign[column];
    }
    return result;
}

core::Vec3f ConvertDirection(const core::Vec3f& source)
{
    return { source.x, source.y, -source.z };
}

core::Vec4f ConvertTangent(const core::Vec4f& source)
{
    return { source.x, source.y, -source.z, -source.w };
}

bool IsSupportedRequiredExtension(const char* extension)
{
    static const std::unordered_set<std::string> supported = {
        "KHR_materials_emissive_strength",
        "KHR_materials_unlit",
        "KHR_texture_basisu",
        "KHR_texture_transform",
        "EXT_texture_webp"
    };
    return extension && supported.contains(extension);
}

bool PreflightParsedData(
    const cgltf_data& data,
    const std::filesystem::path& sourcePath,
    const GltfImportLimits& limits,
    GltfImportStatus& status,
    std::string& error)
{
    for (cgltf_size i = 0; i < data.extensions_required_count; ++i)
    {
        if (!IsSupportedRequiredExtension(data.extensions_required[i]))
        {
            status = GltfImportStatus::UnsupportedFeature;
            error = std::string("required glTF extension is not supported: ") +
                    data.extensions_required[i];
            return false;
        }
    }
    for (cgltf_size i = 0; i < data.buffer_views_count; ++i)
    {
        if (data.buffer_views[i].has_meshopt_compression)
        {
            status = GltfImportStatus::UnsupportedFeature;
            error = "EXT_meshopt_compression is not supported";
            return false;
        }
    }

    uint64_t totalBufferBytes = 0;
    for (cgltf_size i = 0; i < data.buffers_count; ++i)
    {
        const cgltf_buffer& buffer = data.buffers[i];
        if (!CheckedAdd(totalBufferBytes, static_cast<uint64_t>(buffer.size), totalBufferBytes) ||
            totalBufferBytes > limits.maxBufferBytes)
        {
            status = GltfImportStatus::ResourceLimitExceeded;
            error = "buffer payload exceeds the configured import limit";
            return false;
        }
        if (!IsControlledRelativeUri(buffer.uri))
        {
            status = GltfImportStatus::ValidationError;
            error = "buffer URI escapes the controlled asset directory";
            return false;
        }
        if (buffer.uri && !IsDataUri(buffer.uri) && sourcePath.empty())
        {
            status = GltfImportStatus::BufferLoadError;
            error = "memory glTF with external buffers requires an explicit virtual source path";
            return false;
        }
    }

    uint64_t totalEmbeddedImageBytes = 0;
    for (cgltf_size i = 0; i < data.images_count; ++i)
    {
        const cgltf_image& image = data.images[i];
        if (!IsControlledRelativeUri(image.uri))
        {
            status = GltfImportStatus::ValidationError;
            error = "image URI escapes the controlled asset directory";
            return false;
        }
        if (image.uri && !IsDataUri(image.uri) && sourcePath.empty())
        {
            status = GltfImportStatus::BufferLoadError;
            error = "memory glTF with external images requires an explicit virtual source path";
            return false;
        }
        if (image.buffer_view &&
            (!CheckedAdd(totalEmbeddedImageBytes,
                         static_cast<uint64_t>(image.buffer_view->size),
                         totalEmbeddedImageBytes) ||
             totalEmbeddedImageBytes > limits.maxEmbeddedImageBytes))
        {
            status = GltfImportStatus::ResourceLimitExceeded;
            error = "embedded image payload exceeds the configured import limit";
            return false;
        }
    }
    return true;
}

const cgltf_image* TextureImage(const cgltf_texture* texture)
{
    if (!texture)
        return nullptr;
    if (texture->has_basisu && texture->basisu_image)
        return texture->basisu_image;
    if (texture->image)
        return texture->image;
    if (texture->has_webp && texture->webp_image)
        return texture->webp_image;
    return nullptr;
}

TextureBinding ImportTextureBinding(const cgltf_data& data, const cgltf_texture_view& view)
{
    TextureBinding binding;
    if (!view.texture || !TextureImage(view.texture))
        return binding;

    binding.textureIndex = static_cast<uint32_t>(cgltf_texture_index(&data, view.texture));
    binding.texCoord = static_cast<uint32_t>((std::max)(view.texcoord, 0));
    binding.strength = view.scale;

    if (view.has_transform)
    {
        binding.offset = { view.transform.offset[0], view.transform.offset[1] };
        binding.scale = { view.transform.scale[0], view.transform.scale[1] };
        binding.rotation = view.transform.rotation;
        if (view.transform.has_texcoord)
            binding.texCoord = static_cast<uint32_t>((std::max)(view.transform.texcoord, 0));
    }
    return binding;
}

void ImportSamplersAndTextures(const cgltf_data& data, ImportedModel& model)
{
    model.samplers.reserve(data.samplers_count);
    for (cgltf_size i = 0; i < data.samplers_count; ++i)
    {
        const cgltf_sampler& source = data.samplers[i];
        ImportedSampler sampler;
        if (source.name)
            sampler.name = source.name;
        sampler.minFilter = static_cast<TextureFilter>(source.min_filter);
        sampler.magFilter = static_cast<TextureFilter>(source.mag_filter);
        sampler.wrapU = static_cast<TextureWrap>(source.wrap_s);
        sampler.wrapV = static_cast<TextureWrap>(source.wrap_t);
        model.samplers.push_back(std::move(sampler));
    }

    model.textures.reserve(data.textures_count);
    for (cgltf_size i = 0; i < data.textures_count; ++i)
    {
        const cgltf_texture& source = data.textures[i];
        ImportedTexture texture;
        if (source.name)
            texture.name = source.name;
        if (const cgltf_image* image = TextureImage(&source))
            texture.imageIndex = static_cast<uint32_t>(cgltf_image_index(&data, image));
        if (source.sampler)
            texture.samplerIndex = static_cast<uint32_t>(cgltf_sampler_index(&data, source.sampler));
        model.textures.push_back(std::move(texture));
    }
}

void ImportImages(const cgltf_data& data, ImportedModel& model)
{
    model.images.reserve(data.images_count);
    for (cgltf_size i = 0; i < data.images_count; ++i)
    {
        const cgltf_image& source = data.images[i];
        ImageSource image;
        if (source.name)
            image.name = source.name;
        if (source.uri)
            image.uri = source.uri;
        if (source.mime_type)
            image.mimeType = source.mime_type;

        if (source.buffer_view)
        {
            const uint8_t* bytes = cgltf_buffer_view_data(source.buffer_view);
            if (bytes && source.buffer_view->size > 0)
                image.embeddedBytes.assign(bytes, bytes + source.buffer_view->size);
        }
        model.images.push_back(std::move(image));
    }
}

void ImportMaterials(const cgltf_data& data, ImportedModel& model)
{
    model.materials.reserve(data.materials_count);
    for (cgltf_size i = 0; i < data.materials_count; ++i)
    {
        const cgltf_material& source = data.materials[i];
        ImportedMaterial material;
        if (source.name)
            material.name = source.name;

        if (source.has_pbr_metallic_roughness)
        {
            const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
            material.baseColor = {
                pbr.base_color_factor[0], pbr.base_color_factor[1],
                pbr.base_color_factor[2], pbr.base_color_factor[3]
            };
            material.roughness = pbr.roughness_factor;
            material.metallic = pbr.metallic_factor;
            material.baseColorTexture = ImportTextureBinding(data, pbr.base_color_texture);
            material.metallicRoughnessTexture =
                ImportTextureBinding(data, pbr.metallic_roughness_texture);
        }

        material.normalTexture = ImportTextureBinding(data, source.normal_texture);
        material.occlusionTexture = ImportTextureBinding(data, source.occlusion_texture);
        material.emissiveTexture = ImportTextureBinding(data, source.emissive_texture);
        material.emissive = {
            source.emissive_factor[0], source.emissive_factor[1],
            source.emissive_factor[2], 1.0f
        };
        material.emissiveStrength = source.has_emissive_strength
            ? source.emissive_strength.emissive_strength
            : 1.0f;
        material.alphaCutoff = source.alpha_cutoff;
        material.doubleSided = source.double_sided != 0;
        material.unlit = source.unlit != 0;

        switch (source.alpha_mode)
        {
        case cgltf_alpha_mode_mask: material.alphaMode = AlphaMode::Mask; break;
        case cgltf_alpha_mode_blend: material.alphaMode = AlphaMode::Blend; break;
        default: material.alphaMode = AlphaMode::Opaque; break;
        }

        model.materials.push_back(std::move(material));
    }
}

bool IsUnitRange(float value)
{
    return IsFinite(value) && value >= 0.0f && value <= 1.0f;
}

bool ValidateTextureBinding(
    const TextureBinding& binding,
    const ImportedModel& model,
    std::string& error)
{
    if (!binding.IsValid())
        return true;
    if (binding.textureIndex >= model.textures.size())
    {
        error = "material references a texture outside the texture table";
        return false;
    }
    if (binding.texCoord > 1)
    {
        error = "texture binding uses unsupported TEXCOORD set " +
                std::to_string(binding.texCoord);
        return false;
    }
    if (!IsFinite(binding.strength) || !IsFinite(binding.offset) ||
        !IsFinite(binding.scale) || !IsFinite(binding.rotation))
    {
        error = "texture binding contains a non-finite value";
        return false;
    }
    return true;
}

bool ValidateMaterials(const ImportedModel& model, std::string& error)
{
    for (const ImportedMaterial& material : model.materials)
    {
        if (!IsUnitRange(material.baseColor.r) || !IsUnitRange(material.baseColor.g) ||
            !IsUnitRange(material.baseColor.b) || !IsUnitRange(material.baseColor.a) ||
            !IsUnitRange(material.roughness) || !IsUnitRange(material.metallic) ||
            !IsUnitRange(material.emissive.r) || !IsUnitRange(material.emissive.g) ||
            !IsUnitRange(material.emissive.b) || !IsUnitRange(material.alphaCutoff) ||
            !IsFinite(material.emissiveStrength) || material.emissiveStrength < 0.0f)
        {
            error = "material contains a non-finite or out-of-range factor";
            return false;
        }

        const TextureBinding* bindings[] = {
            &material.baseColorTexture,
            &material.normalTexture,
            &material.metallicRoughnessTexture,
            &material.occlusionTexture,
            &material.emissiveTexture
        };
        for (const TextureBinding* binding : bindings)
        {
            if (!ValidateTextureBinding(*binding, model, error))
                return false;
        }
        if (material.occlusionTexture.IsValid() &&
            !IsUnitRange(material.occlusionTexture.strength))
        {
            error = "occlusion texture strength is outside [0, 1]";
            return false;
        }
    }
    return true;
}

const cgltf_accessor* FindAttribute(
    const cgltf_primitive& primitive,
    cgltf_attribute_type type,
    cgltf_int index = 0)
{
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
    {
        const cgltf_attribute& attribute = primitive.attributes[i];
        if (attribute.type == type && attribute.index == index)
            return attribute.data;
    }
    return nullptr;
}

bool UnpackAttribute(
    const cgltf_accessor* accessor,
    cgltf_type requiredType,
    cgltf_size expectedCount,
    std::vector<float>& values,
    std::string& error,
    const char* semantic)
{
    if (!accessor)
        return true;
    if (accessor->type != requiredType || accessor->count != expectedCount)
    {
        std::ostringstream stream;
        stream << semantic << " accessor has the wrong shape or vertex count";
        error = stream.str();
        return false;
    }

    const cgltf_size componentCount = cgltf_num_components(accessor->type);
    const cgltf_size floatCount = accessor->count * componentCount;
    values.resize(floatCount);
    if (cgltf_accessor_unpack_floats(accessor, values.data(), floatCount) != floatCount)
    {
        error = std::string("could not unpack ") + semantic + " accessor";
        return false;
    }
    for (float value : values)
    {
        if (!IsFinite(value))
        {
            error = std::string(semantic) + " accessor contains a non-finite value";
            return false;
        }
    }
    return true;
}

bool ImportIndices(
    const cgltf_primitive& source,
    cgltf_size vertexCount,
    bool reverseWinding,
    const GltfImportLimits& limits,
    uint64_t currentIndexCount,
    ImportedPrimitive& destination,
    GltfImportStatus& failureStatus,
    std::string& error)
{
    const uint64_t incomingIndexCount = source.indices
        ? static_cast<uint64_t>(source.indices->count)
        : static_cast<uint64_t>(vertexCount);
    uint64_t totalIndexCount = 0;
    if (!CheckedAdd(currentIndexCount, incomingIndexCount, totalIndexCount) ||
        totalIndexCount > limits.maxIndices)
    {
        failureStatus = GltfImportStatus::ResourceLimitExceeded;
        error = "total index count exceeds the configured import limit";
        return false;
    }

    if (source.indices)
    {
        if (source.indices->type != cgltf_type_scalar)
        {
            error = "index accessor must contain scalar values";
            return false;
        }
        if (source.indices->is_sparse)
        {
            error = "sparse index accessors are not supported";
            return false;
        }
        destination.indices.resize(source.indices->count);
        if (cgltf_accessor_unpack_indices(
                source.indices,
                destination.indices.data(),
                sizeof(uint32_t),
                source.indices->count) != source.indices->count)
        {
            error = "could not unpack index accessor";
            return false;
        }
    }
    else
    {
        destination.indices.resize(vertexCount);
        for (cgltf_size i = 0; i < vertexCount; ++i)
            destination.indices[i] = static_cast<uint32_t>(i);
    }

    if (destination.indices.empty() || destination.indices.size() % 3 != 0)
    {
        error = "triangle primitive index count must be a nonzero multiple of three";
        return false;
    }

    for (uint32_t index : destination.indices)
    {
        if (index >= vertexCount)
        {
            error = "index accessor references a vertex outside POSITION";
            return false;
        }
    }

    if (reverseWinding)
    {
        for (size_t i = 0; i < destination.indices.size(); i += 3)
            std::swap(destination.indices[i + 1], destination.indices[i + 2]);
    }
    return true;
}

void GenerateNormals(ImportedPrimitive& primitive)
{
    for (ImportedVertex& vertex : primitive.vertices)
        vertex.normal = {};

    for (size_t i = 0; i < primitive.indices.size(); i += 3)
    {
        ImportedVertex& a = primitive.vertices[primitive.indices[i + 0]];
        ImportedVertex& b = primitive.vertices[primitive.indices[i + 1]];
        ImportedVertex& c = primitive.vertices[primitive.indices[i + 2]];
        const core::Vec3f face = (b.position - a.position).Cross(c.position - a.position);
        a.normal += face;
        b.normal += face;
        c.normal += face;
    }

    for (ImportedVertex& vertex : primitive.vertices)
        vertex.normal = vertex.normal.Normalized();
}

bool GenerateTangents(
    ImportedPrimitive& primitive,
    uint32_t texCoord,
    std::string& error)
{
    std::vector<core::Vec3f> tangentSums(primitive.vertices.size());
    std::vector<core::Vec3f> bitangentSums(primitive.vertices.size());
    const auto uvFor = [texCoord](const ImportedVertex& vertex) -> const core::Vec2f& {
        return texCoord == 0 ? vertex.uv0 : vertex.uv1;
    };

    for (size_t i = 0; i < primitive.indices.size(); i += 3)
    {
        const uint32_t ia = primitive.indices[i + 0];
        const uint32_t ib = primitive.indices[i + 1];
        const uint32_t ic = primitive.indices[i + 2];
        const ImportedVertex& a = primitive.vertices[ia];
        const ImportedVertex& b = primitive.vertices[ib];
        const ImportedVertex& c = primitive.vertices[ic];
        const core::Vec3f edge1 = b.position - a.position;
        const core::Vec3f edge2 = c.position - a.position;
        const core::Vec2f delta1 = uvFor(b) - uvFor(a);
        const core::Vec2f delta2 = uvFor(c) - uvFor(a);
        const float denominator = delta1.x * delta2.y - delta1.y * delta2.x;
        if (std::fabs(denominator) < 1e-12f)
            continue;

        const float reciprocal = 1.0f / denominator;
        const core::Vec3f tangent =
            (edge1 * delta2.y - edge2 * delta1.y) * reciprocal;
        const core::Vec3f bitangent =
            (edge2 * delta1.x - edge1 * delta2.x) * reciprocal;
        tangentSums[ia] += tangent;
        tangentSums[ib] += tangent;
        tangentSums[ic] += tangent;
        bitangentSums[ia] += bitangent;
        bitangentSums[ib] += bitangent;
        bitangentSums[ic] += bitangent;
    }

    for (size_t i = 0; i < primitive.vertices.size(); ++i)
    {
        ImportedVertex& vertex = primitive.vertices[i];
        core::Vec3f tangent =
            tangentSums[i] - vertex.normal * tangentSums[i].Dot(vertex.normal);
        tangent = tangent.Normalized();
        if (tangent.LengthSq() < 1e-12f)
        {
            const core::Vec3f helper = std::fabs(vertex.normal.z) < 0.999f
                ? core::Vec3f{ 0.0f, 0.0f, 1.0f }
                : core::Vec3f{ 0.0f, 1.0f, 0.0f };
            tangent = helper.Cross(vertex.normal).Normalized();
        }
        if (!IsFinite(tangent) || tangent.LengthSq() < 1e-12f)
        {
            error = "cannot generate a finite tangent basis";
            return false;
        }
        const float handedness = bitangentSums[i].LengthSq() < 1e-12f
            ? 1.0f
            : (vertex.normal.Cross(tangent).Dot(bitangentSums[i]) < 0.0f ? -1.0f : 1.0f);
        vertex.tangent = { tangent.x, tangent.y, tangent.z, handedness };
    }
    return true;
}

uint8_t MaterialTexcoordMask(const ImportedMaterial& material)
{
    uint8_t mask = 0;
    const TextureBinding* bindings[] = {
        &material.baseColorTexture,
        &material.normalTexture,
        &material.metallicRoughnessTexture,
        &material.occlusionTexture,
        &material.emissiveTexture
    };
    for (const TextureBinding* binding : bindings)
    {
        if (binding->IsValid())
            mask |= static_cast<uint8_t>(1u << binding->texCoord);
    }
    return mask;
}

bool ImportPrimitive(
    const cgltf_data& data,
    const cgltf_node& node,
    const cgltf_mesh& mesh,
    cgltf_size primitiveIndex,
    const core::Mat4x4& transform,
    const GltfImportLimits& limits,
    ImportedModel& model,
    GltfImportStatus& failureStatus,
    std::string& error)
{
    const cgltf_primitive& source = mesh.primitives[primitiveIndex];
    if (source.has_draco_mesh_compression)
    {
        failureStatus = GltfImportStatus::UnsupportedFeature;
        error = "KHR_draco_mesh_compression is not supported";
        return false;
    }
    if (source.type != cgltf_primitive_type_triangles)
    {
        failureStatus = GltfImportStatus::UnsupportedFeature;
        error = "only triangle-list glTF primitives are supported";
        return false;
    }

    const cgltf_accessor* positions =
        FindAttribute(source, cgltf_attribute_type_position);
    if (!positions || positions->type != cgltf_type_vec3 || positions->count == 0)
    {
        error = "triangle primitive is missing a nonempty VEC3 POSITION accessor";
        return false;
    }
    uint64_t totalVertexCount = 0;
    if (!CheckedAdd(model.VertexCount(), static_cast<uint64_t>(positions->count),
                    totalVertexCount) ||
        totalVertexCount > limits.maxVertices)
    {
        failureStatus = GltfImportStatus::ResourceLimitExceeded;
        error = "vertex count exceeds the configured import limit";
        return false;
    }

    const cgltf_accessor* normals = FindAttribute(source, cgltf_attribute_type_normal);
    const cgltf_accessor* texcoords0 = FindAttribute(source, cgltf_attribute_type_texcoord, 0);
    const cgltf_accessor* texcoords1 = FindAttribute(source, cgltf_attribute_type_texcoord, 1);
    const cgltf_accessor* tangents = FindAttribute(source, cgltf_attribute_type_tangent);
    const cgltf_accessor* colors = FindAttribute(source, cgltf_attribute_type_color);

    std::vector<float> positionValues;
    std::vector<float> normalValues;
    std::vector<float> texcoord0Values;
    std::vector<float> texcoord1Values;
    std::vector<float> tangentValues;
    std::vector<float> colorValues;
    if (!UnpackAttribute(positions, cgltf_type_vec3, positions->count,
                         positionValues, error, "POSITION") ||
        !UnpackAttribute(normals, cgltf_type_vec3, positions->count,
                         normalValues, error, "NORMAL") ||
        !UnpackAttribute(texcoords0, cgltf_type_vec2, positions->count,
                         texcoord0Values, error, "TEXCOORD_0") ||
        !UnpackAttribute(texcoords1, cgltf_type_vec2, positions->count,
                         texcoord1Values, error, "TEXCOORD_1") ||
        !UnpackAttribute(tangents, cgltf_type_vec4, positions->count,
                         tangentValues, error, "TANGENT"))
    {
        return false;
    }

    cgltf_size colorComponents = 0;
    if (colors)
    {
        if ((colors->type != cgltf_type_vec3 && colors->type != cgltf_type_vec4) ||
            colors->count != positions->count)
        {
            error = "COLOR_0 accessor has the wrong shape or vertex count";
            return false;
        }
        colorComponents = cgltf_num_components(colors->type);
        const cgltf_size count = colors->count * colorComponents;
        colorValues.resize(count);
        if (cgltf_accessor_unpack_floats(colors, colorValues.data(), count) != count)
        {
            error = "could not unpack COLOR_0 accessor";
            return false;
        }
    }

    ImportedPrimitive primitive;
    primitive.nodeName = node.name ? node.name : "node";
    const std::string meshName = mesh.name ? mesh.name : "mesh";
    primitive.name = meshName + ":" + std::to_string(primitiveIndex);
    primitive.vertices.resize(positions->count);
    if (source.material)
        primitive.materialIndex = static_cast<uint32_t>(cgltf_material_index(&data, source.material));

    uint8_t requiredTexcoordMask = 0;
    if (primitive.materialIndex != kInvalidAssetIndex)
        requiredTexcoordMask = MaterialTexcoordMask(model.materials[primitive.materialIndex]);
    const uint8_t availableTexcoordMask =
        static_cast<uint8_t>((texcoords0 ? 1u : 0u) | (texcoords1 ? 2u : 0u));
    if ((requiredTexcoordMask & availableTexcoordMask) != requiredTexcoordMask)
    {
        error = "material references a texture coordinate set missing from its primitive";
        return false;
    }

    const float determinant = Determinant3x3(transform);
    if (std::fabs(determinant) < 1e-12f)
    {
        error = "node transform is singular";
        return false;
    }
    const bool reverseWinding = determinant >= 0.0f;
    if (!ImportIndices(source, positions->count, reverseWinding, limits,
                       model.IndexCount(), primitive, failureStatus, error))
        return false;

    const core::Mat4x4 normalTransform = core::Mat4x4::InverseTranspose3x3(transform);
    for (cgltf_size i = 0; i < positions->count; ++i)
    {
        ImportedVertex& vertex = primitive.vertices[i];
        const core::Vec3f localPosition = {
            positionValues[i * 3 + 0],
            positionValues[i * 3 + 1],
            -positionValues[i * 3 + 2]
        };
        vertex.position = transform.TransformPoint(localPosition);

        if (normals)
        {
            const core::Vec3f localNormal = ConvertDirection({
                normalValues[i * 3 + 0],
                normalValues[i * 3 + 1],
                normalValues[i * 3 + 2]
            });
            vertex.normal = normalTransform.TransformDirection(localNormal).Normalized();
        }

        if (texcoords0)
        {
            vertex.uv0 = { texcoord0Values[i * 2 + 0], texcoord0Values[i * 2 + 1] };
            vertex.texcoordMask |= 1u;
        }
        if (texcoords1)
        {
            vertex.uv1 = { texcoord1Values[i * 2 + 0], texcoord1Values[i * 2 + 1] };
            vertex.texcoordMask |= 2u;
        }

        if (colors)
        {
            vertex.color = {
                colorValues[i * colorComponents + 0],
                colorValues[i * colorComponents + 1],
                colorValues[i * colorComponents + 2],
                colorComponents == 4 ? colorValues[i * colorComponents + 3] : 1.0f
            };
        }

        if (tangents)
        {
            core::Vec4f localTangent = ConvertTangent({
                tangentValues[i * 4 + 0], tangentValues[i * 4 + 1],
                tangentValues[i * 4 + 2], tangentValues[i * 4 + 3]
            });
            core::Vec3f tangent = transform.TransformDirection(localTangent.XYZ());
            if (normals)
                tangent = tangent - vertex.normal * tangent.Dot(vertex.normal);
            tangent = tangent.Normalized();
            vertex.tangent = {
                tangent.x, tangent.y, tangent.z,
                determinant < 0.0f ? -localTangent.w : localTangent.w
            };
        }

        if (!IsFinite(vertex.position) || !IsFinite(vertex.normal) ||
            !IsFinite(vertex.uv0) || !IsFinite(vertex.uv1) ||
            !IsFinite(vertex.color) || !IsFinite(vertex.tangent))
        {
            error = "converted vertex contains a non-finite value";
            return false;
        }
        primitive.bounds.Expand(vertex.position);
    }

    if (!normals)
        GenerateNormals(primitive);

    if (!tangents)
    {
        uint32_t tangentTexcoord = 0;
        bool needsTangents = false;
        if (primitive.materialIndex != kInvalidAssetIndex)
        {
            const TextureBinding& normalTexture =
                model.materials[primitive.materialIndex].normalTexture;
            needsTangents = normalTexture.IsValid();
            tangentTexcoord = normalTexture.texCoord;
        }
        if (needsTangents &&
            !GenerateTangents(primitive, tangentTexcoord, error))
        {
            return false;
        }
    }

    model.bounds.Expand(primitive.bounds);
    model.primitives.push_back(std::move(primitive));
    return true;
}

bool ImportNode(
    const cgltf_data& data,
    const cgltf_node& node,
    uint32_t depth,
    const GltfImportLimits& limits,
    ImportedModel& model,
    std::vector<std::string>& warnings,
    GltfImportStatus& failureStatus,
    std::string& error)
{
    if (depth > limits.maxNodeDepth)
    {
        failureStatus = GltfImportStatus::ResourceLimitExceeded;
        error = "node hierarchy exceeds the configured depth limit";
        return false;
    }

    if (node.skin)
        warnings.push_back(std::string("node '") + (node.name ? node.name : "node") +
                           "' has a skin; importing its static bind-pose mesh only");

    if (node.mesh)
    {
        if (model.primitives.size() + node.mesh->primitives_count > limits.maxPrimitives)
        {
            failureStatus = GltfImportStatus::ResourceLimitExceeded;
            error = "primitive count exceeds the configured import limit";
            return false;
        }

        cgltf_float sourceTransform[16];
        cgltf_node_transform_world(&node, sourceTransform);
        const core::Mat4x4 transform = ToEngineMatrix(sourceTransform);
        for (cgltf_size i = 0; i < node.mesh->primitives_count; ++i)
        {
            if (!ImportPrimitive(data, node, *node.mesh, i, transform, limits,
                                 model, failureStatus, error))
                return false;
        }
    }

    for (cgltf_size i = 0; i < node.children_count; ++i)
    {
        if (!ImportNode(data, *node.children[i], depth + 1, limits, model,
                        warnings, failureStatus, error))
            return false;
    }
    return true;
}

bool BindExternalBufferSnapshots(
    cgltf_data& data,
    std::span<const GltfExternalBuffer> externalBuffers,
    std::string& error)
{
    static const std::byte emptyBufferSentinel{};
    for (cgltf_size i = 0; i < data.buffers_count; ++i)
    {
        cgltf_buffer& buffer = data.buffers[i];
        if (!buffer.uri || IsDataUri(buffer.uri))
            continue;

        const std::string decodedUri = DecodeUri(buffer.uri);
        const auto snapshot = std::find_if(
            externalBuffers.begin(), externalBuffers.end(),
            [&decodedUri](const GltfExternalBuffer& candidate) {
                return candidate.uri == decodedUri;
            });
        if (snapshot == externalBuffers.end())
        {
            error = "external buffer snapshot is missing: " + decodedUri;
            return false;
        }
        if (snapshot->bytes.size() < buffer.size)
        {
            error = "external buffer snapshot is shorter than its declared size: " + decodedUri;
            return false;
        }
        buffer.data = snapshot->bytes.empty()
            ? const_cast<std::byte*>(&emptyBufferSentinel)
            : const_cast<std::byte*>(snapshot->bytes.data());
        buffer.data_free_method = cgltf_data_free_method_none;
    }
    return true;
}

GltfImportResult ImportParsedData(
    CgltfDataPtr data,
    const std::filesystem::path& sourcePath,
    const GltfImportLimits& limits,
    std::span<const GltfExternalBuffer> externalBuffers = {},
    bool requireExternalBufferSnapshots = false)
{
    GltfImportStatus preflightStatus = GltfImportStatus::ValidationError;
    std::string preflightError;
    if (!PreflightParsedData(*data, sourcePath, limits, preflightStatus, preflightError))
        return Failure(preflightStatus, std::move(preflightError));

    if (requireExternalBufferSnapshots)
    {
        std::string snapshotError;
        if (!BindExternalBufferSnapshots(*data, externalBuffers, snapshotError))
            return Failure(GltfImportStatus::BufferLoadError, std::move(snapshotError));
    }

    cgltf_options options = {};
    const std::string pathString = sourcePath.empty() ? std::string() : sourcePath.string();
    cgltf_result loadResult = cgltf_load_buffers(&options, data.get(), pathString.c_str());
    if (loadResult != cgltf_result_success)
    {
        return Failure(
            GltfImportStatus::BufferLoadError,
            std::string("glTF buffer load failed: ") + CgltfResultName(loadResult));
    }

    const cgltf_result validationResult = cgltf_validate(data.get());
    if (validationResult != cgltf_result_success)
    {
        return Failure(
            GltfImportStatus::ValidationError,
            std::string("glTF validation failed: ") + CgltfResultName(validationResult));
    }

    GltfImportResult result;
    result.status = GltfImportStatus::Success;
    result.model.name = sourcePath.empty() ? "memory" : sourcePath.stem().string();
    std::string dependencyError;
    if (!CollectSourceDependencies(*data, result.sourceDependencies, dependencyError))
        return Failure(GltfImportStatus::ValidationError, std::move(dependencyError));
    ImportImages(*data, result.model);
    ImportSamplersAndTextures(*data, result.model);
    ImportMaterials(*data, result.model);

    std::string materialError;
    if (!ValidateMaterials(result.model, materialError))
        return Failure(GltfImportStatus::ValidationError, std::move(materialError));

    std::string importError;
    GltfImportStatus importStatus = GltfImportStatus::InvalidGeometry;
    const cgltf_scene* scene = data->scene;
    if (!scene && data->scenes_count > 0)
        scene = &data->scenes[0];

    if (scene)
    {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i)
        {
            if (!ImportNode(*data, *scene->nodes[i], 0, limits,
                            result.model, result.warnings, importStatus, importError))
            {
                return Failure(importStatus, std::move(importError));
            }
        }
    }
    else
    {
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
        {
            if (data->nodes[i].parent)
                continue;
            if (!ImportNode(*data, data->nodes[i], 0, limits,
                            result.model, result.warnings, importStatus, importError))
            {
                return Failure(importStatus, std::move(importError));
            }
        }
    }

    if (result.model.primitives.empty())
        return Failure(GltfImportStatus::InvalidGeometry, "glTF contains no importable mesh primitives");

    return result;
}

GltfImportResult ImportGltfMemoryInternal(
    std::span<const std::byte> bytes,
    const std::filesystem::path& virtualSourcePath,
    const GltfImportLimits& limits,
    std::span<const GltfExternalBuffer> externalBuffers,
    bool requireExternalBufferSnapshots)
{
    if (bytes.empty())
        return Failure(GltfImportStatus::ParseError, "glTF source is empty");
    if (bytes.size() > limits.maxSourceBytes)
        return Failure(GltfImportStatus::SourceTooLarge, "glTF source exceeds the configured size limit");

    cgltf_options options = {};
    cgltf_data* rawData = nullptr;
    const cgltf_result parseResult =
        cgltf_parse(&options, bytes.data(), bytes.size(), &rawData);
    if (parseResult != cgltf_result_success)
    {
        return Failure(
            GltfImportStatus::ParseError,
            std::string("glTF parse failed: ") + CgltfResultName(parseResult));
    }
    return ImportParsedData(
        CgltfDataPtr(rawData), virtualSourcePath, limits,
        externalBuffers, requireExternalBufferSnapshots);
}

} // namespace

const char* GltfImportStatusName(GltfImportStatus status)
{
    switch (status)
    {
    case GltfImportStatus::Success: return "success";
    case GltfImportStatus::FileNotFound: return "file not found";
    case GltfImportStatus::IoError: return "I/O error";
    case GltfImportStatus::SourceTooLarge: return "source too large";
    case GltfImportStatus::ParseError: return "parse error";
    case GltfImportStatus::BufferLoadError: return "buffer load error";
    case GltfImportStatus::ValidationError: return "validation error";
    case GltfImportStatus::UnsupportedFeature: return "unsupported feature";
    case GltfImportStatus::InvalidGeometry: return "invalid geometry";
    case GltfImportStatus::ResourceLimitExceeded: return "resource limit exceeded";
    default: return "unknown";
    }
}

GltfImportResult ImportGltfFile(
    const std::filesystem::path& path,
    const GltfImportLimits& limits)
{
    std::error_code errorCode;
    if (!std::filesystem::exists(path, errorCode))
        return Failure(GltfImportStatus::FileNotFound, "glTF source file does not exist");
    if (errorCode)
        return Failure(GltfImportStatus::IoError, "could not inspect glTF source file");

    const uint64_t fileSize = std::filesystem::file_size(path, errorCode);
    if (errorCode)
        return Failure(GltfImportStatus::IoError, "could not determine glTF source size");
    if (fileSize == 0)
        return Failure(GltfImportStatus::ParseError, "glTF source file is empty");
    if (fileSize > limits.maxSourceBytes)
        return Failure(GltfImportStatus::SourceTooLarge, "glTF source exceeds the configured size limit");

    std::vector<std::byte> bytes(static_cast<size_t>(fileSize));
    std::ifstream stream(path, std::ios::binary);
    if (!stream || !stream.read(reinterpret_cast<char*>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size())))
    {
        return Failure(GltfImportStatus::IoError, "could not read glTF source file");
    }
    return ImportGltfMemory(bytes, path, limits);
}

GltfImportResult ImportGltfMemory(
    std::span<const std::byte> bytes,
    const std::filesystem::path& virtualSourcePath,
    const GltfImportLimits& limits)
{
    return ImportGltfMemoryInternal(bytes, virtualSourcePath, limits, {}, false);
}

GltfImportResult ImportGltfMemoryWithExternalBuffers(
    std::span<const std::byte> bytes,
    const std::filesystem::path& virtualSourcePath,
    std::span<const GltfExternalBuffer> externalBuffers,
    const GltfImportLimits& limits)
{
    return ImportGltfMemoryInternal(
        bytes, virtualSourcePath, limits, externalBuffers, true);
}

GltfDependencyScanResult ScanGltfSourceDependencies(
    std::span<const std::byte> bytes,
    const GltfImportLimits& limits)
{
    GltfDependencyScanResult result;
    if (bytes.empty())
    {
        result.error = "glTF source is empty";
        return result;
    }
    if (bytes.size() > limits.maxSourceBytes)
    {
        result.status = GltfImportStatus::SourceTooLarge;
        result.error = "glTF source exceeds the configured size limit";
        return result;
    }

    cgltf_options options = {};
    cgltf_data* rawData = nullptr;
    const cgltf_result parseResult =
        cgltf_parse(&options, bytes.data(), bytes.size(), &rawData);
    if (parseResult != cgltf_result_success)
    {
        result.error = std::string("glTF parse failed: ") + CgltfResultName(parseResult);
        return result;
    }
    CgltfDataPtr data(rawData);
    if (!CollectSourceDependencies(*data, result.dependencies, result.error))
    {
        result.status = GltfImportStatus::ValidationError;
        return result;
    }
    result.status = GltfImportStatus::Success;
    return result;
}

} // namespace asset
