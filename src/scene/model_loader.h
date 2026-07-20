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
// SCOPE OF THIS SLICE: geometry and scalar material factors only. Embedded
// images are NOT decoded or uploaded yet, so materials use their baseColor /
// roughness / metallic / emissive factors with no texture maps. That is a
// deliberate first slice: it proves the geometry path end to end (the hard,
// valuable part) without the image-decode-from-memory work, which follows.

#include "scene.h"
#include "../ecs/components.h"
#include "../render/d3d12_device.h"

namespace render { class Renderer; }

#include <cstdint>
#include <filesystem>
#include <vector>
#include <wrl/client.h>

namespace scene
{

struct LoadedModel
{
    // Entities spawned, one per imported primitive. Empty on failure.
    std::vector<ecs::Entity> entities;

    // Upload buffers backing the mesh copies. The CPU->GPU copy is recorded onto
    // the caller's command list but does not complete until the caller flushes
    // and waits, so these MUST outlive that wait. The caller keeps this struct
    // alive until after its WaitForGpu, exactly as InitializeScene already does
    // for the procedural demo meshes.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadBuffers;

    bool ok = false;
    std::string error;
};

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

} // namespace scene
