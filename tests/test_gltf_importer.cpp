#include "test_framework.h"
#include "asset/gltf_importer.h"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{

struct GlbOptions
{
    bool use32BitIndices = true;
    bool includeIndices = true;
    bool includeNormals = true;
    bool includeTangents = true;
    bool includeUv1 = true;
    float metallicFactor = 0.8f;
    uint32_t thirdIndex = 2;
    uint32_t primitiveMode = 4;
    bool rotateRoot = false;
    bool useRootMatrix = false;
    bool mirrorChild = false;
};

template<typename T>
void AppendValue(std::vector<std::byte>& bytes, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const size_t offset = bytes.size();
    bytes.resize(offset + sizeof(T));
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void AppendVertex(
    std::vector<std::byte>& bytes,
    const std::array<float, 3>& position,
    const std::array<float, 3>& normal,
    const std::array<float, 2>& uv,
    const std::array<float, 2>& uv1,
    const std::array<float, 4>& tangent,
    const std::array<float, 4>& color)
{
    for (float value : position) AppendValue(bytes, value);
    for (float value : normal) AppendValue(bytes, value);
    for (float value : uv) AppendValue(bytes, value);
    for (float value : uv1) AppendValue(bytes, value);
    for (float value : tangent) AppendValue(bytes, value);
    for (float value : color) AppendValue(bytes, value);
}

std::vector<std::byte> BuildGlb(const GlbOptions& options = {})
{
    constexpr float invSqrt2 = 0.70710678118f;
    std::vector<std::byte> binary;
    AppendVertex(binary, { 0.0f, 0.0f, 0.0f }, { 0.0f, -invSqrt2, invSqrt2 },
                 { 0.0f, 0.0f }, { 0.25f, 0.25f },
                 { 1.0f, 0.0f, 0.0f, 1.0f },
                 { 1.0f, 0.0f, 0.0f, 1.0f });
    AppendVertex(binary, { 1.0f, 0.0f, 0.0f }, { 0.0f, -invSqrt2, invSqrt2 },
                 { 1.0f, 0.0f }, { 0.75f, 0.25f },
                 { 1.0f, 0.0f, 0.0f, 1.0f },
                 { 0.0f, 1.0f, 0.0f, 1.0f });
    AppendVertex(binary, { 0.0f, 1.0f, 1.0f }, { 0.0f, -invSqrt2, invSqrt2 },
                 { 0.0f, 1.0f }, { 0.25f, 0.75f },
                 { 1.0f, 0.0f, 0.0f, 1.0f },
                 { 0.0f, 0.0f, 1.0f, 0.5f });

    const uint32_t indexOffset = static_cast<uint32_t>(binary.size());
    if (options.use32BitIndices)
    {
        AppendValue(binary, uint32_t{ 0 });
        AppendValue(binary, uint32_t{ 1 });
        AppendValue(binary, options.thirdIndex);
    }
    else
    {
        AppendValue(binary, uint16_t{ 0 });
        AppendValue(binary, uint16_t{ 1 });
        AppendValue(binary, static_cast<uint16_t>(options.thirdIndex));
    }
    const uint32_t indexLength = options.use32BitIndices ? 12u : 6u;

    while (binary.size() % 4 != 0)
        binary.push_back(std::byte{ 0 });
    const uint32_t imageOffset = static_cast<uint32_t>(binary.size());
    binary.push_back(std::byte{ 0x89 });
    binary.push_back(std::byte{ 0x50 });
    binary.push_back(std::byte{ 0x4e });
    binary.push_back(std::byte{ 0x47 });

    std::ostringstream json;
    json << R"({"asset":{"version":"2.0"},)"
         << R"("extensionsUsed":["KHR_texture_transform","KHR_materials_emissive_strength"],)"
         << R"("extensionsRequired":["KHR_texture_transform","KHR_materials_emissive_strength"],)"
         << R"("buffers":[{"byteLength":)" << binary.size() << R"(}],)"
         << R"("bufferViews":[)"
         << R"({"buffer":0,"byteOffset":0,"byteLength":216,"byteStride":72,"target":34962},)"
         << R"({"buffer":0,"byteOffset":)" << indexOffset
         << R"(,"byteLength":)" << indexLength << R"(,"target":34963},)"
         << R"({"buffer":0,"byteOffset":)" << imageOffset
         << R"(,"byteLength":4}],)"
         << R"("accessors":[)"
         << R"({"bufferView":0,"byteOffset":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,1]},)"
         << R"({"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC3"},)"
         << R"({"bufferView":0,"byteOffset":24,"componentType":5126,"count":3,"type":"VEC2"},)"
         << R"({"bufferView":0,"byteOffset":32,"componentType":5126,"count":3,"type":"VEC2"},)"
         << R"({"bufferView":0,"byteOffset":40,"componentType":5126,"count":3,"type":"VEC4"},)"
         << R"({"bufferView":0,"byteOffset":56,"componentType":5126,"count":3,"type":"VEC4"},)"
         << R"({"bufferView":1,"componentType":)" << (options.use32BitIndices ? 5125 : 5123)
         << R"(,"count":3,"type":"SCALAR"}],)"
         << R"("images":[{"name":"Tiny","bufferView":2,"mimeType":"image/png"}],)"
         << R"("samplers":[{"name":"HullSampler","magFilter":9729,"minFilter":9987,"wrapS":33071,"wrapT":33648}],)"
         << R"("textures":[{"name":"HullTexture","source":0,"sampler":0}],)"
         << R"("materials":[{"name":"Hull","pbrMetallicRoughness":{)"
         << R"("baseColorFactor":[0.2,0.4,0.6,1],"metallicFactor":)"
         << options.metallicFactor << R"(,"roughnessFactor":0.3,)"
         << R"("baseColorTexture":{"index":0,"texCoord":0,"extensions":{"KHR_texture_transform":{)"
         << R"("offset":[0.1,0.2],"scale":[2,3],"rotation":0.25,"texCoord":1}}},)"
         << R"("metallicRoughnessTexture":{"index":0}},)"
         << R"("normalTexture":{"index":0,"scale":0.7},)"
         << R"("occlusionTexture":{"index":0,"strength":0.6},)"
         << R"("emissiveTexture":{"index":0},"emissiveFactor":[0.1,0.2,0.3],)"
         << R"("alphaMode":"MASK","alphaCutoff":0.25,"doubleSided":true,)"
         << R"("extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":4}}}],)"
         << R"("meshes":[{"name":"TestMesh","primitives":[{"attributes":{"POSITION":0)";
    if (options.includeNormals)
        json << R"(,"NORMAL":1)";
    json << R"(,"TEXCOORD_0":2)";
    if (options.includeUv1)
        json << R"(,"TEXCOORD_1":3)";
    if (options.includeTangents)
        json << R"(,"TANGENT":4)";
    json << R"(,"COLOR_0":5})";
    if (options.includeIndices)
        json << R"(,"indices":6)";
    json << R"(,"material":0,"mode":)" << options.primitiveMode << R"(}]}],)"
         << R"("nodes":[{"name":"Root")";
    if (options.useRootMatrix)
    {
        json << R"(,"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,10,2,-5,1])";
    }
    else
    {
        json << R"(,"translation":[10,2,-5])";
        if (options.rotateRoot)
            json << R"(,"rotation":[0,0.70710678118,0,0.70710678118])";
    }
    json << R"(,"children":[1]},)"
         << R"({"name":"HullNode","scale":[)"
         << (options.mirrorChild ? -2 : 2)
         << R"(,1,0.5],"mesh":0}],)"
         << R"("scenes":[{"name":"Main","nodes":[0]}],"scene":0})";

    std::string jsonText = json.str();
    while (jsonText.size() % 4 != 0)
        jsonText.push_back(' ');
    while (binary.size() % 4 != 0)
        binary.push_back(std::byte{ 0 });

    constexpr uint32_t jsonChunkType = 0x4e4f534a;
    constexpr uint32_t binChunkType = 0x004e4942;
    const uint32_t totalLength = 12u + 8u + static_cast<uint32_t>(jsonText.size())
                               + 8u + static_cast<uint32_t>(binary.size());

    std::vector<std::byte> glb;
    AppendValue(glb, uint32_t{ 0x46546c67 });
    AppendValue(glb, uint32_t{ 2 });
    AppendValue(glb, totalLength);
    AppendValue(glb, static_cast<uint32_t>(jsonText.size()));
    AppendValue(glb, jsonChunkType);
    const size_t jsonOffset = glb.size();
    glb.resize(glb.size() + jsonText.size());
    std::memcpy(glb.data() + jsonOffset, jsonText.data(), jsonText.size());
    AppendValue(glb, static_cast<uint32_t>(binary.size()));
    AppendValue(glb, binChunkType);
    glb.insert(glb.end(), binary.begin(), binary.end());
    return glb;
}

std::string Base64Encode(const std::vector<std::byte>& bytes)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        const uint32_t a = std::to_integer<uint8_t>(bytes[i]);
        const uint32_t b = i + 1 < bytes.size() ? std::to_integer<uint8_t>(bytes[i + 1]) : 0;
        const uint32_t c = i + 2 < bytes.size() ? std::to_integer<uint8_t>(bytes[i + 2]) : 0;
        const uint32_t packed = (a << 16) | (b << 8) | c;
        encoded.push_back(alphabet[(packed >> 18) & 63]);
        encoded.push_back(alphabet[(packed >> 12) & 63]);
        encoded.push_back(i + 1 < bytes.size() ? alphabet[(packed >> 6) & 63] : '=');
        encoded.push_back(i + 2 < bytes.size() ? alphabet[packed & 63] : '=');
    }
    return encoded;
}

std::vector<std::byte> BuildTextGltf(bool duplicatePrimitive = false)
{
    std::vector<std::byte> binary;
    for (float value : std::array<float, 9>{
             0.0f, 0.0f, 0.0f,
             1.0f, 0.0f, 0.0f,
             0.0f, 1.0f, 1.0f })
    {
        AppendValue(binary, value);
    }
    AppendValue(binary, uint16_t{ 0 });
    AppendValue(binary, uint16_t{ 1 });
    AppendValue(binary, uint16_t{ 2 });

    std::ostringstream json;
    json << R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":42,"uri":)"
         << '"' << "data:application/octet-stream;base64," << Base64Encode(binary) << '"'
         << R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
         << R"({"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[)"
         << R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,1]},)"
         << R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],)"
         << R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1})";
    if (duplicatePrimitive)
        json << R"(,{"attributes":{"POSITION":0},"indices":1})";
    json << R"(]}],)"
         << R"("nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";

    const std::string text = json.str();
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::vector<std::byte> BuildExternalBufferGltf(std::string_view uri)
{
    const std::string text =
        std::string(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36,"uri":")") +
        std::string(uri) +
        R"("}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

} // namespace

TEST_CASE(GltfImporter_RejectsMissingAndMalformedSources)
{
    const asset::GltfImportResult missing =
        asset::ImportGltfFile(std::filesystem::path("definitely_missing_asset.glb"));
    CHECK_FALSE(missing.Succeeded());
    CHECK_EQ(missing.status, asset::GltfImportStatus::FileNotFound);

    const std::array<std::byte, 5> malformed = {
        std::byte{ '{' }, std::byte{ 'n' }, std::byte{ 'o' },
        std::byte{ 'p' }, std::byte{ 'e' }
    };
    const asset::GltfImportResult parsed = asset::ImportGltfMemory(malformed);
    CHECK_FALSE(parsed.Succeeded());
    CHECK_EQ(parsed.status, asset::GltfImportStatus::ParseError);
}

TEST_CASE(GltfImporter_ImportsGlbGeometryAndBakesHierarchy)
{
    const std::vector<std::byte> glb = BuildGlb();
    const asset::GltfImportResult result =
        asset::ImportGltfMemory(glb, std::filesystem::path("test_ship.glb"));

    CHECK(result.Succeeded());
    CHECK_EQ(result.model.name, std::string("test_ship"));
    CHECK_EQ(result.model.primitives.size(), size_t{ 1 });
    CHECK_EQ(result.model.VertexCount(), uint64_t{ 3 });
    CHECK_EQ(result.model.IndexCount(), uint64_t{ 3 });

    const asset::ImportedPrimitive& primitive = result.model.primitives[0];
    CHECK_EQ(primitive.name, std::string("TestMesh:0"));
    CHECK_EQ(primitive.nodeName, std::string("HullNode"));
    CHECK_EQ(primitive.indices[0], uint32_t{ 0 });
    CHECK_EQ(primitive.indices[1], uint32_t{ 2 });
    CHECK_EQ(primitive.indices[2], uint32_t{ 1 });

    CHECK_APPROX(primitive.vertices[0].position.x, 10.0f);
    CHECK_APPROX(primitive.vertices[0].position.y, 2.0f);
    CHECK_APPROX(primitive.vertices[0].position.z, 5.0f);
    CHECK_APPROX(primitive.vertices[1].position.x, 12.0f);
    CHECK_APPROX(primitive.vertices[2].position.y, 3.0f);
    CHECK_APPROX(primitive.vertices[2].position.z, 4.5f);

    CHECK_APPROX_EPS(primitive.vertices[0].normal.x, 0.0f, 1e-5f);
    CHECK_APPROX_EPS(primitive.vertices[0].normal.y, -0.4472136f, 1e-5f);
    CHECK_APPROX_EPS(primitive.vertices[0].normal.z, -0.8944272f, 1e-5f);
    CHECK_APPROX(primitive.vertices[0].tangent.x, 1.0f);
    CHECK_APPROX(primitive.vertices[0].tangent.w, -1.0f);
    CHECK_APPROX(primitive.vertices[2].color.b, 1.0f);
    CHECK_APPROX(primitive.vertices[2].color.a, 0.5f);
    CHECK_APPROX(primitive.vertices[2].uv0.y, 1.0f);
    CHECK_APPROX(primitive.vertices[2].uv1.y, 0.75f);
    CHECK_EQ(primitive.vertices[2].texcoordMask, uint8_t{ 3 });

    CHECK(primitive.bounds.valid);
    CHECK_APPROX(primitive.bounds.min.x, 10.0f);
    CHECK_APPROX(primitive.bounds.max.x, 12.0f);
    CHECK_APPROX(result.model.bounds.min.z, 4.5f);
    CHECK_APPROX(result.model.bounds.max.z, 5.0f);
}

TEST_CASE(GltfImporter_MapsPbrMaterialsImagesAndTextureTransforms)
{
    const asset::GltfImportResult result = asset::ImportGltfMemory(BuildGlb());
    CHECK(result.Succeeded());
    CHECK_EQ(result.model.materials.size(), size_t{ 1 });
    CHECK_EQ(result.model.images.size(), size_t{ 1 });
    CHECK_EQ(result.model.textures.size(), size_t{ 1 });
    CHECK_EQ(result.model.samplers.size(), size_t{ 1 });

    const asset::ImportedMaterial& material = result.model.materials[0];
    CHECK_EQ(material.name, std::string("Hull"));
    CHECK_APPROX(material.baseColor.r, 0.2f);
    CHECK_APPROX(material.baseColor.g, 0.4f);
    CHECK_APPROX(material.baseColor.b, 0.6f);
    CHECK_APPROX(material.metallic, 0.8f);
    CHECK_APPROX(material.roughness, 0.3f);
    CHECK_APPROX(material.emissive.b, 0.3f);
    CHECK_APPROX(material.emissiveStrength, 4.0f);
    CHECK_EQ(material.alphaMode, asset::AlphaMode::Mask);
    CHECK_APPROX(material.alphaCutoff, 0.25f);
    CHECK(material.doubleSided);
    CHECK(material.baseColorTexture.IsValid());
    CHECK_EQ(material.baseColorTexture.textureIndex, uint32_t{ 0 });
    CHECK_EQ(material.baseColorTexture.texCoord, uint32_t{ 1 });
    CHECK_APPROX(material.baseColorTexture.offset.x, 0.1f);
    CHECK_APPROX(material.baseColorTexture.scale.y, 3.0f);
    CHECK_APPROX(material.baseColorTexture.rotation, 0.25f);
    CHECK_APPROX(material.normalTexture.strength, 0.7f);
    CHECK_APPROX(material.occlusionTexture.strength, 0.6f);

    const asset::ImportedTexture& texture = result.model.textures[0];
    CHECK_EQ(texture.name, std::string("HullTexture"));
    CHECK_EQ(texture.imageIndex, uint32_t{ 0 });
    CHECK_EQ(texture.samplerIndex, uint32_t{ 0 });
    const asset::ImportedSampler& sampler = result.model.samplers[0];
    CHECK_EQ(sampler.wrapU, asset::TextureWrap::ClampToEdge);
    CHECK_EQ(sampler.wrapV, asset::TextureWrap::MirroredRepeat);
    CHECK_EQ(sampler.minFilter, asset::TextureFilter::LinearMipmapLinear);

    const asset::ImageSource& image = result.model.images[0];
    CHECK_EQ(image.name, std::string("Tiny"));
    CHECK_EQ(image.mimeType, std::string("image/png"));
    CHECK(image.IsEmbedded());
    CHECK_EQ(image.embeddedBytes.size(), size_t{ 4 });
    CHECK_EQ(image.embeddedBytes[0], uint8_t{ 0x89 });
}

TEST_CASE(GltfImporter_ImportsTextGltfWithDataUriBuffer)
{
    const asset::GltfImportResult result = asset::ImportGltfMemory(
        BuildTextGltf(), std::filesystem::path("triangle.gltf"));
    CHECK(result.Succeeded());
    CHECK_EQ(result.model.primitives.size(), size_t{ 1 });
    CHECK_EQ(result.model.primitives[0].vertices.size(), size_t{ 3 });
    CHECK_EQ(result.model.primitives[0].indices[1], uint32_t{ 2 });
    CHECK_APPROX(result.model.primitives[0].vertices[2].position.z, -1.0f);
}

TEST_CASE(GltfImporter_ConvertsRotatedNodeHierarchy)
{
    GlbOptions options;
    options.rotateRoot = true;
    const asset::GltfImportResult result = asset::ImportGltfMemory(BuildGlb(options));
    CHECK(result.Succeeded());

    const core::Vec3f position = result.model.primitives[0].vertices[1].position;
    CHECK_APPROX_EPS(position.x, 10.0f, 1e-5f);
    CHECK_APPROX_EPS(position.y, 2.0f, 1e-5f);
    CHECK_APPROX_EPS(position.z, 7.0f, 1e-5f);
}

TEST_CASE(GltfImporter_ConvertsRawNodeMatrix)
{
    GlbOptions options;
    options.useRootMatrix = true;
    const asset::GltfImportResult result = asset::ImportGltfMemory(BuildGlb(options));
    CHECK(result.Succeeded());

    const asset::ImportedPrimitive& primitive = result.model.primitives[0];
    CHECK_APPROX(primitive.vertices[0].position.x, 10.0f);
    CHECK_APPROX(primitive.vertices[0].position.y, 2.0f);
    CHECK_APPROX(primitive.vertices[0].position.z, 5.0f);
    CHECK_APPROX(primitive.vertices[1].position.x, 12.0f);
}

TEST_CASE(GltfImporter_PreservesMirroredTransformBasisAndWinding)
{
    GlbOptions options;
    options.mirrorChild = true;
    const asset::GltfImportResult result = asset::ImportGltfMemory(BuildGlb(options));
    CHECK(result.Succeeded());

    const asset::ImportedPrimitive& primitive = result.model.primitives[0];
    CHECK_EQ(primitive.indices[0], uint32_t{ 0 });
    CHECK_EQ(primitive.indices[1], uint32_t{ 1 });
    CHECK_EQ(primitive.indices[2], uint32_t{ 2 });
    CHECK_APPROX(primitive.vertices[1].position.x, 8.0f);
    CHECK_APPROX_EPS(primitive.vertices[0].normal.Length(), 1.0f, 1e-5f);
    CHECK_APPROX_EPS(primitive.vertices[0].tangent.XYZ().Length(), 1.0f, 1e-5f);
    CHECK_APPROX_EPS(
        primitive.vertices[0].normal.Dot(primitive.vertices[0].tangent.XYZ()),
        0.0f,
        1e-5f);
    CHECK_APPROX(primitive.vertices[0].tangent.x, -1.0f);
    CHECK_APPROX(primitive.vertices[0].tangent.w, 1.0f);
}

TEST_CASE(GltfImporter_AcceptsSixteenAndThirtyTwoBitIndices)
{
    GlbOptions sixteenBit;
    sixteenBit.use32BitIndices = false;
    const asset::GltfImportResult u16 = asset::ImportGltfMemory(BuildGlb(sixteenBit));
    CHECK(u16.Succeeded());
    CHECK_EQ(u16.model.primitives[0].indices[1], uint32_t{ 2 });

    GlbOptions thirtyTwoBit;
    thirtyTwoBit.use32BitIndices = true;
    const asset::GltfImportResult u32 = asset::ImportGltfMemory(BuildGlb(thirtyTwoBit));
    CHECK(u32.Succeeded());
    CHECK_EQ(u32.model.primitives[0].indices[1], uint32_t{ 2 });
}

TEST_CASE(GltfImporter_GeneratesNormalsForNonIndexedTriangles)
{
    GlbOptions options;
    options.includeIndices = false;
    options.includeNormals = false;
    const asset::GltfImportResult result = asset::ImportGltfMemory(BuildGlb(options));
    CHECK(result.Succeeded());
    CHECK_EQ(result.model.primitives[0].indices.size(), size_t{ 3 });

    const core::Vec3f normal = result.model.primitives[0].vertices[0].normal;
    CHECK_APPROX_EPS(normal.x, 0.0f, 1e-5f);
    CHECK_APPROX_EPS(normal.y, -0.4472136f, 1e-5f);
    CHECK_APPROX_EPS(normal.z, -0.8944272f, 1e-5f);
}

TEST_CASE(GltfImporter_GeneratesTangentsForNormalMappedMesh)
{
    GlbOptions options;
    options.includeTangents = false;
    const asset::GltfImportResult result = asset::ImportGltfMemory(BuildGlb(options));
    CHECK(result.Succeeded());

    const core::Vec4f tangent = result.model.primitives[0].vertices[0].tangent;
    CHECK(tangent.XYZ().LengthSq() > 0.99f);
    CHECK_APPROX_EPS(
        tangent.XYZ().Dot(result.model.primitives[0].vertices[0].normal),
        0.0f,
        1e-5f);
    CHECK(std::fabs(tangent.w) == 1.0f);
}

TEST_CASE(GltfImporter_RejectsMissingMaterialTexcoordSet)
{
    GlbOptions options;
    options.includeUv1 = false;
    const asset::GltfImportResult result = asset::ImportGltfMemory(BuildGlb(options));
    CHECK_FALSE(result.Succeeded());
    CHECK_EQ(result.status, asset::GltfImportStatus::InvalidGeometry);
    CHECK(result.error.find("texture coordinate set") != std::string::npos);
}

TEST_CASE(GltfImporter_RejectsUnsafeExternalDependencyUris)
{
    const asset::GltfImportResult noBase =
        asset::ImportGltfMemory(BuildExternalBufferGltf("mesh.bin"));
    CHECK_FALSE(noBase.Succeeded());
    CHECK_EQ(noBase.status, asset::GltfImportStatus::BufferLoadError);

    const asset::GltfImportResult traversal = asset::ImportGltfMemory(
        BuildExternalBufferGltf("%2e%2e/mesh.bin"),
        std::filesystem::path("C:/safe/model.gltf"));
    CHECK_FALSE(traversal.Succeeded());
    CHECK_EQ(traversal.status, asset::GltfImportStatus::ValidationError);
}

TEST_CASE(GltfImporter_ReportsDecodedExternalBufferDependencies)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "the_dawning_gltf_dependency_test";
    std::error_code errorCode;
    std::filesystem::remove_all(directory, errorCode);
    std::filesystem::create_directories(directory, errorCode);
    CHECK_FALSE(static_cast<bool>(errorCode));
    if (errorCode)
        return;

    const std::array<float, 9> positions = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };
    const std::filesystem::path bufferPath = directory / "mesh.bin";
    {
        std::ofstream stream(bufferPath, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(positions.data()), sizeof(positions));
        CHECK(static_cast<bool>(stream));
    }

    const asset::GltfImportResult result = asset::ImportGltfMemory(
        BuildExternalBufferGltf("%6d%65sh.bin"), directory / "model.gltf");
    CHECK(result.Succeeded());
    if (result.Succeeded())
    {
        CHECK_EQ(result.sourceDependencies.size(), size_t{ 1 });
        if (!result.sourceDependencies.empty())
        {
            CHECK_EQ(result.sourceDependencies[0].uri, std::string("mesh.bin"));
            CHECK_EQ(result.sourceDependencies[0].kind, asset::GltfDependencyKind::Buffer);
        }
    }
    std::filesystem::remove_all(directory, errorCode);
}

TEST_CASE(GltfImporter_RejectsOutOfRangeMaterialFactors)
{
    GlbOptions options;
    options.metallicFactor = 1.5f;
    const asset::GltfImportResult result = asset::ImportGltfMemory(BuildGlb(options));
    CHECK_FALSE(result.Succeeded());
    CHECK_EQ(result.status, asset::GltfImportStatus::ValidationError);
    CHECK(result.error.find("out-of-range") != std::string::npos);
}

TEST_CASE(GltfImporter_NegativeTestsRejectUnsupportedModeAndBadIndex)
{
    GlbOptions strip;
    strip.primitiveMode = 5;
    const asset::GltfImportResult unsupported = asset::ImportGltfMemory(BuildGlb(strip));
    CHECK_FALSE(unsupported.Succeeded());
    CHECK(unsupported.error.find("triangle-list") != std::string::npos);

    GlbOptions badIndex;
    badIndex.thirdIndex = 99;
    const asset::GltfImportResult invalid = asset::ImportGltfMemory(BuildGlb(badIndex));
    CHECK_FALSE(invalid.Succeeded());
    CHECK(invalid.status == asset::GltfImportStatus::ValidationError ||
          invalid.error.find("outside POSITION") != std::string::npos);
}

TEST_CASE(GltfImporter_EnforcesConfiguredResourceLimits)
{
    asset::GltfImportLimits limits;
    limits.maxVertices = 2;
    const asset::GltfImportResult result = asset::ImportGltfMemory(BuildGlb(), {}, limits);
    CHECK_FALSE(result.Succeeded());
    CHECK_EQ(result.status, asset::GltfImportStatus::ResourceLimitExceeded);
    CHECK(result.error.find("vertex count") != std::string::npos);

    limits = {};
    limits.maxBufferBytes = 100;
    const asset::GltfImportResult buffers =
        asset::ImportGltfMemory(BuildGlb(), {}, limits);
    CHECK_FALSE(buffers.Succeeded());
    CHECK_EQ(buffers.status, asset::GltfImportStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxEmbeddedImageBytes = 3;
    const asset::GltfImportResult images =
        asset::ImportGltfMemory(BuildGlb(), {}, limits);
    CHECK_FALSE(images.Succeeded());
    CHECK_EQ(images.status, asset::GltfImportStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxIndices = 5;
    const asset::GltfImportResult totalIndices =
        asset::ImportGltfMemory(BuildTextGltf(true), {}, limits);
    CHECK_FALSE(totalIndices.Succeeded());
    CHECK_EQ(totalIndices.status, asset::GltfImportStatus::ResourceLimitExceeded);
    CHECK(totalIndices.error.find("total index count") != std::string::npos);
}
