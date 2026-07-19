#include "model_loader.h"

#include "resource_manager.h"
#include "../asset/gltf_importer.h"
#include "../asset/model_data.h"
#include "../core/log.h"
#include "../render/mesh.h"

#include <cstdint>

namespace scene
{

namespace
{

// asset::ImportedVertex carries more than the engine renders (a second UV set, a
// tangent, a texcoord mask). The engine Vertex is position/normal/color/uv only;
// the raster path derives tangents from screen-space derivatives, so dropping the
// imported tangent here is consistent with how every other mesh in the engine is
// already shaded, not a loss.
render::Vertex ToEngineVertex(const asset::ImportedVertex& in)
{
    render::Vertex out;
    out.position = in.position;
    out.normal   = in.normal;
    out.color    = in.color;
    out.uv       = in.uv0;
    return out;
}

// Scalar factors only in this slice - no texture handles are set, so the
// material renders with its baseColor/roughness/metallic/emissive constants and
// UINT32_MAX (no map) everywhere else. The importer already converted colours and
// factors into the engine's conventions, so this is a straight copy.
ecs::Material ToEngineMaterial(const asset::ImportedMaterial& in)
{
    ecs::Material out;
    out.albedo    = in.baseColor;
    out.roughness = in.roughness;
    out.metallic  = in.metallic;
    out.emissive  = in.emissive;
    // glTF defaults emissiveStrength to 1.0 even for a black emissive; the engine
    // defaults it to 0.0. Copying the glTF value is correct - black * 1.0 is still
    // black, and a real emitter keeps its intended strength.
    out.emissiveStrength = in.emissiveStrength;
    return out;
}

} // namespace

LoadedModel LoadModelIntoScene(Scene& scene,
                               render::D3D12Device& device,
                               const std::filesystem::path& path,
                               const ecs::Transform& baseTransform)
{
    LoadedModel loaded;

    const asset::GltfImportResult imported = asset::ImportGltfFile(path);
    if (!imported.Succeeded())
    {
        loaded.error = imported.error.empty() ? "glTF import failed" : imported.error;
        core::Log::Errorf("Model load failed for %s: %s",
                          path.string().c_str(), loaded.error.c_str());
        return loaded;
    }
    for (const std::string& warning : imported.warnings)
        core::Log::Infof("Model load warning (%s): %s",
                         path.string().c_str(), warning.c_str());

    const asset::ImportedModel& model = imported.model;
    ResourceManager& resources = scene.GetResources();

    ID3D12Device* d3dDevice = device.Device();
    ID3D12GraphicsCommandList* cmd = device.CmdList();

    uint64_t totalVertices = 0;
    uint64_t totalIndices  = 0;

    for (size_t p = 0; p < model.primitives.size(); ++p)
    {
        const asset::ImportedPrimitive& prim = model.primitives[p];
        if (prim.vertices.empty() || prim.indices.empty())
            continue;

        std::vector<render::Vertex> vertices;
        vertices.reserve(prim.vertices.size());
        for (const asset::ImportedVertex& v : prim.vertices)
            vertices.push_back(ToEngineVertex(v));

        // CreateMesh32, not CreateMesh: the importer emits 32-bit indices and a
        // real asset (the corridor is ~15.5k vertices, others exceed 65535) will
        // not fit the 16-bit path. CreateMesh32 also sets indexFormat to
        // R32_UINT, so the mesh carries its own width.
        Microsoft::WRL::ComPtr<ID3D12Resource> vbUpload;
        Microsoft::WRL::ComPtr<ID3D12Resource> ibUpload;
        render::Mesh mesh = render::CreateMesh32(
            d3dDevice, cmd,
            vertices.data(), static_cast<uint32_t>(vertices.size()),
            prim.indices.data(), static_cast<uint32_t>(prim.indices.size()),
            vbUpload, ibUpload);

        if (!mesh.IsValid())
        {
            loaded.error = "GPU mesh creation failed for a primitive";
            core::Log::Errorf("Model load: CreateMesh32 failed for %s primitive %zu",
                              path.string().c_str(), p);
            return loaded;
        }

        // The upload buffers must survive until the caller flushes and waits.
        loaded.uploadBuffers.push_back(std::move(vbUpload));
        loaded.uploadBuffers.push_back(std::move(ibUpload));

        const std::string meshName =
            prim.name.empty() ? (model.name + "_prim" + std::to_string(p)) : prim.name;
        const MeshHandle meshHandle = resources.AddMesh(std::move(mesh), meshName.c_str());

        ecs::Material material;
        if (prim.materialIndex != asset::kInvalidAssetIndex &&
            prim.materialIndex < model.materials.size())
        {
            material = ToEngineMaterial(model.materials[prim.materialIndex]);
        }

        const ecs::Entity entity =
            scene.CreateRenderable(meshName.c_str(), meshHandle, material, baseTransform);
        loaded.entities.push_back(entity);

        totalVertices += prim.vertices.size();
        totalIndices  += prim.indices.size();
    }

    if (loaded.entities.empty())
    {
        loaded.error = "model contained no renderable primitives";
        core::Log::Errorf("Model load: %s produced no primitives", path.string().c_str());
        return loaded;
    }

    loaded.ok = true;
    // Structured marker so the smoke harness can assert a generated asset actually
    // reached the GPU, not merely that the importer parsed it. Counts, not prose.
    core::Log::Infof("[SMOKE] model_loaded=ok primitives=%zu vertices=%llu indices=%llu",
                     loaded.entities.size(),
                     static_cast<unsigned long long>(totalVertices),
                     static_cast<unsigned long long>(totalIndices));
    return loaded;
}

} // namespace scene
