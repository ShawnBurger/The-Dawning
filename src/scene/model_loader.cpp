#include "model_loader.h"

#include "resource_manager.h"
#include "../asset/cooked_model.h"
#include "../asset/gltf_importer.h"
#include "../asset/model_data.h"
#include "../core/log.h"
#include "../render/mesh.h"
#include "../render/renderer.h"
#include "../render/texture.h"

#include <cstdint>
#include <string>

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

// Resolve a glTF texture binding to the engine texture handle for the image it
// ultimately references. The chain is: material binding -> textures[] entry ->
// imageIndex -> the decoded engine texture. Returns UINT32_MAX (no map) if the
// binding is unset or its image failed to decode.
uint32_t ResolveBinding(const asset::ImportedModel& model,
                        const asset::TextureBinding& binding,
                        const std::vector<uint32_t>& imageHandles)
{
    if (binding.textureIndex == asset::kInvalidAssetIndex ||
        binding.textureIndex >= model.textures.size())
        return UINT32_MAX;
    const uint32_t imageIndex = model.textures[binding.textureIndex].imageIndex;
    if (imageIndex == asset::kInvalidAssetIndex || imageIndex >= imageHandles.size())
        return UINT32_MAX;
    return imageHandles[imageIndex];
}

// Map material factors and resolve its texture bindings to engine handles.
// imageHandles is indexed by glTF image index; entries are UINT32_MAX where an
// image was absent or failed to decode, which the shaders already treat as "no
// map" via the useXxxTexture gates.
ecs::Material ToEngineMaterial(const asset::ImportedModel& model,
                               const asset::ImportedMaterial& in,
                               const std::vector<uint32_t>& imageHandles)
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

    out.albedoTextureHandle   = ResolveBinding(model, in.baseColorTexture, imageHandles);
    out.normalTextureHandle   = ResolveBinding(model, in.normalTexture, imageHandles);
    // glTF's metallicRoughness texture is exactly the engine's ORM packing on the
    // two channels that matter: roughness in G, metallic in B. The engine also
    // reads occlusion from R; glTF leaves R to a separate occlusion texture, so
    // when only metallicRoughness is present R is whatever the exporter wrote.
    // That at worst perturbs the ambient term slightly - G and B, the channels
    // that drive the actual BRDF, are correct - which is far better than dropping
    // per-texel roughness and metallic entirely. Prefer a dedicated occlusion
    // texture's image when the exporter provided a combined one, else fall back
    // to metallicRoughness.
    uint32_t ormHandle = ResolveBinding(model, in.metallicRoughnessTexture, imageHandles);
    if (in.occlusionTexture.textureIndex != asset::kInvalidAssetIndex)
    {
        const uint32_t occ = ResolveBinding(model, in.occlusionTexture, imageHandles);
        if (occ != UINT32_MAX)
            ormHandle = occ;  // combined ORM authored in the occlusion slot
    }
    out.ormTextureHandle      = ormHandle;
    out.emissiveTextureHandle = ResolveBinding(model, in.emissiveTexture, imageHandles);
    return out;
}

LoadedModel UploadImportedModel(Scene& scene,
                                render::D3D12Device& device,
                                render::Renderer& renderer,
                                const asset::ImportedModel& model,
                                const std::string& sourceLabel,
                                const char* sourceKind,
                                const ecs::Transform& baseTransform)
{
    LoadedModel loaded;
    ResourceManager& resources = scene.GetResources();

    ID3D12Device* d3dDevice = device.Device();
    ID3D12GraphicsCommandList* cmd = device.CmdList();
    std::vector<TextureHandle> registeredTextures;
    std::vector<MeshHandle> registeredMeshes;

    const auto rollback = [&]()
    {
        for (ecs::Entity entity : loaded.entities)
            scene.DestroyEntity(entity);
        loaded.entities.clear();
        for (MeshHandle handle : registeredMeshes)
            resources.RemoveMesh(handle, device);
        for (TextureHandle handle : registeredTextures)
            resources.RemoveTexture(handle, device, renderer);
        registeredMeshes.clear();
        registeredTextures.clear();
    };

    // Decode each embedded image ONCE, up front, and register it with both the
    // raster descriptor heap (RegisterTexture, which stamps texture.descriptor)
    // and the ResourceManager (which both render paths look meshes' materials up
    // through). imageHandles is parallel to model.images; a failed or external
    // image stays UINT32_MAX so the material treats it as "no map".
    //
    // A model with M images and one material consumes M raster descriptor slots.
    // The raster table is kMaxRasterTextures wide and the DXR side 64 per channel,
    // so a kit of many textured assets will exhaust these - see the texture-table
    // ceiling in ASSET_PIPELINE_SPEC.md. For a single asset it is fine.
    std::vector<uint32_t> imageHandles(model.images.size(), UINT32_MAX);
    uint32_t decodedImages = 0;
    for (size_t i = 0; i < model.images.size(); ++i)
    {
        const asset::ImageSource& img = model.images[i];
        if (img.embeddedBytes.empty())
        {
            // External-URI images are not fetched in this slice; the importer
            // already confined any URI to the controlled asset directory, but
            // decoding one would mean a filesystem read this path does not do.
            core::Log::Infof("Model load: image %zu has no embedded bytes; skipped", i);
            continue;
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> texUpload;
        render::Texture tex = render::CreateTexture2DFromWICMemory(
            d3dDevice, cmd,
            img.embeddedBytes.data(), img.embeddedBytes.size(),
            texUpload, L"ImportedImage");
        if (!tex.IsValid())
        {
            core::Log::Warnf("Model load: image %zu failed to decode; material will "
                             "fall back to its scalar factor", i);
            continue;
        }

        tex.descriptor = renderer.RegisterTexture(d3dDevice, tex);
        if (!tex.descriptor.IsValid())
        {
            loaded.error = "texture descriptor allocation failed";
            core::Log::Errorf("Model load: descriptor allocation failed for image %zu", i);
            rollback();
            return loaded;
        }
        loaded.uploadBuffers.push_back(std::move(texUpload));

        const std::string texName =
            img.name.empty() ? (model.name + "_img" + std::to_string(i)) : img.name;
        const TextureHandle handle =
            resources.AddTexture(std::move(tex), texName.c_str());
        if (!handle.IsValid())
        {
            loaded.error = "texture registration failed";
            renderer.ReleaseTextureDescriptor(device, tex.descriptor);
            device.DeferredRelease(tex.resource);
            tex.ResetAfterRetirement();
            rollback();
            return loaded;
        }
        registeredTextures.push_back(handle);
        imageHandles[i] = handle.value;
        ++decodedImages;
    }

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
                              sourceLabel.c_str(), p);
            rollback();
            return loaded;
        }

        // The upload buffers must survive until the caller flushes and waits.
        loaded.uploadBuffers.push_back(std::move(vbUpload));
        loaded.uploadBuffers.push_back(std::move(ibUpload));

        const std::string meshName =
            prim.name.empty() ? (model.name + "_prim" + std::to_string(p)) : prim.name;
        const MeshHandle meshHandle = resources.AddMesh(std::move(mesh), meshName.c_str());
        if (!meshHandle.IsValid())
        {
            loaded.error = "mesh registration failed";
            device.DeferredRelease(mesh.vertexBuffer);
            device.DeferredRelease(mesh.indexBuffer);
            rollback();
            return loaded;
        }
        registeredMeshes.push_back(meshHandle);

        ecs::Material material;
        if (prim.materialIndex != asset::kInvalidAssetIndex &&
            prim.materialIndex < model.materials.size())
        {
            material = ToEngineMaterial(model, model.materials[prim.materialIndex],
                                        imageHandles);
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
        core::Log::Errorf("Model load: %s produced no primitives", sourceLabel.c_str());
        rollback();
        return loaded;
    }

    loaded.ok = true;
    // The source kind distinguishes the production cooked path from a raw glTF
    // development load; counts make a tiny substitute asset fail the same gate.
    core::Log::Infof("[SMOKE] model_loaded=ok model_source=%s model_primitives=%zu model_vertices=%llu model_indices=%llu model_images=%u",
                     sourceKind,
                     loaded.entities.size(),
                     static_cast<unsigned long long>(totalVertices),
                     static_cast<unsigned long long>(totalIndices),
                     decodedImages);
    return loaded;
}

} // namespace

LoadedModel LoadModelIntoScene(Scene& scene,
                               render::D3D12Device& device,
                               render::Renderer& renderer,
                               const std::filesystem::path& path,
                               const ecs::Transform& baseTransform)
{
    const asset::GltfImportResult imported = asset::ImportGltfFile(path);
    if (!imported.Succeeded())
    {
        LoadedModel loaded;
        loaded.error = imported.error.empty() ? "glTF import failed" : imported.error;
        core::Log::Errorf("Model load failed for %s: %s",
                          path.string().c_str(), loaded.error.c_str());
        return loaded;
    }
    for (const std::string& warning : imported.warnings)
        core::Log::Infof("Model load warning (%s): %s",
                         path.string().c_str(), warning.c_str());

    return UploadImportedModel(scene, device, renderer, imported.model,
                               path.string(), "gltf", baseTransform);
}

LoadedModel LoadCookedModelIntoScene(Scene& scene,
                                     render::D3D12Device& device,
                                     render::Renderer& renderer,
                                     const std::filesystem::path& path,
                                     const ecs::Transform& baseTransform)
{
    const asset::CookedModelResult cooked = asset::LoadCookedModelFile(path);
    if (!cooked.Succeeded())
    {
        LoadedModel loaded;
        loaded.error = cooked.error.empty()
            ? asset::CookedModelStatusName(cooked.status)
            : cooked.error;
        core::Log::Errorf("Cooked model load failed for %s [%s]: %s",
                          path.string().c_str(),
                          asset::CookedModelStatusName(cooked.status),
                          loaded.error.c_str());
        return loaded;
    }

    core::Log::Infof("Cooked model verified: %s source_sha256=%s dependencies=%zu",
                     path.string().c_str(),
                     cooked.metadata.sourceSha256.Hex().c_str(),
                     cooked.metadata.dependencies.size());
    return UploadImportedModel(scene, device, renderer, cooked.model,
                               path.string(), "cooked", baseTransform);
}

} // namespace scene
