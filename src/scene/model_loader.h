#pragma once

// Bridges a CPU-side asset::ImportedModel onto the GPU and into the scene.
//
// The glTF importer (src/asset) produces an ImportedModel and Codex's asset
// compiler cooks it to a binary, but neither uploads anything - Codex's Stage 3
// handoff explicitly defers "runtime GPU upload". That deferred step is this
// file: ImportedModel primitives become render::Mesh objects, materials become
// ecs::Material, and one entity is spawned per primitive. It is the point where
// a generated asset stops being data and starts being something on screen.
//
// Source glTF remains available for tools and asset iteration. Production
// content loads the versioned engine-owned .tdmodel format and then enters the
// exact same upload path, including embedded PBR images.

#include "scene.h"
#include "../asset/cooked_model.h"
#include "../ecs/components.h"
#include "../render/d3d12_device.h"

namespace render { class Renderer; }

#include <cstdint>
#include <filesystem>
#include <vector>
#include <wrl/client.h>

namespace scene
{

struct LoadedModelPrimitive
{
    uint32_t sourcePrimitiveIndex = 0;
    MeshHandle mesh;
    ecs::Material material;
    std::string name;
};

struct LoadedModelResources
{
    std::vector<LoadedModelPrimitive> primitives;
    std::vector<TextureHandle> textures;

    // Upload buffers back CPU->GPU copies recorded on the caller's open command
    // list. They may be released only after that command list has retired.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadBuffers;

    uint64_t vertexCount = 0;
    uint64_t indexCount = 0;
    uint32_t decodedImageCount = 0;
    asset::Sha256Digest sourceSha256;
    bool ok = false;
    std::string error;
};

struct LoadedModel
{
    // Entities spawned, one per imported primitive. Empty on failure.
    std::vector<ecs::Entity> entities;

    // Upload buffers backing the mesh copies. The CPU->GPU copy is recorded onto
    // the caller's command list but does not complete until the caller flushes
    // and waits, so these MUST outlive that wait. The caller keeps this struct
    // alive until after its WaitForGpu, exactly as InitializeScene already does
    // for the procedural demo meshes.
    LoadedModelResources resources;

    bool ok = false;
    std::string error;
};

// Load and register one model's GPU resources without creating ECS entities.
// The caller owns the returned handles and must either transfer them to a
// longer-lived runtime owner or release them explicitly with
// ReleaseLoadedModelResources before Scene shutdown.
LoadedModelResources LoadCookedModelResources(
    Scene& scene,
    render::D3D12Device& device,
    render::Renderer& renderer,
    const std::filesystem::path& path);

void ReleaseLoadedModelResources(
    LoadedModelResources& resources,
    Scene& scene,
    render::D3D12Device& device,
    render::Renderer& renderer) noexcept;

// Load a glTF/GLB from disk, record its geometry uploads onto device's CURRENTLY
// OPEN command list, register the meshes with the scene's ResourceManager, and
// spawn one renderable entity per primitive under baseTransform.
//
// Does NOT close, execute, or wait on the command list - the caller batches this
// with its other uploads and flushes once, which is why uploadBuffers comes back
// in the result rather than being freed here. Rendering does not begin until the
// frame loop, well after that flush, so spawning the entities here is safe.
LoadedModel LoadModelIntoScene(Scene& scene,
                               render::D3D12Device& device,
                               render::Renderer& renderer,
                               const std::filesystem::path& path,
                               const ecs::Transform& baseTransform = {});

// Load an offline-cooked .tdmodel without parsing source glTF/JSON at runtime,
// then feed its ImportedModel payload through the same GPU/material bridge.
LoadedModel LoadCookedModelIntoScene(Scene& scene,
                                     render::D3D12Device& device,
                                     render::Renderer& renderer,
                                     const std::filesystem::path& path,
                                     const ecs::Transform& baseTransform = {});

} // namespace scene
