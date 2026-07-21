#pragma once
// =============================================================================
// scene/resource_manager.h — Handle-Based Resource Management
// =============================================================================
// Manages GPU resources (meshes, materials) via generational handles.
// ECS components store handles, never raw pointers or GPU addresses.
//
// Features:
//   - Slot-map pools with O(1) lookup by handle
//   - Generational handles catch stale CPU-side handle reuse
//   - Multiple entities can share the same mesh/material
//
// GPU release is deferred and fence-guarded.
//   RemoveMesh and RemoveTexture hand their GPU objects to
//   D3D12Device::DeferredRelease rather than dropping the last ComPtr on the
//   spot. The device retains each one until the GPU has passed the fence value
//   signalled at the end of the frame in which it was retired, then frees it.
//   Both are therefore safe to call mid-frame, which is the normal case since
//   the device runs up to kFrameCount frames in flight.
//
//   This replaces a real use-after-free: overwriting the slot released the
//   resource immediately while recorded-but-not-yet-retired command lists could
//   still reference it. The generational handle never protected against that —
//   it invalidates future CPU lookups, and the GPU does not consult it.
//
//   RemoveTexture also returns the shader-visible descriptor slot that
//   Renderer::RegisterTexture allocated, via Renderer::ReleaseTextureDescriptor.
//   That slot is fence-guarded the same way the resource is: it is parked until
//   the GPU has retired every command list that could still reference it, then
//   recycled. Texture churn no longer exhausts the heap.
//
//   RemoveMaterial needs neither — MaterialData is CPU-only and holds no GPU
//   objects and no descriptors.
// =============================================================================

#include "../render/mesh.h"
#include "../render/texture.h"
#include "../core/types.h"
#include "../render/d3d12_device.h"   // for D3D12Device, whose fence-guarded
                                       // queue owns deferred GPU release
#include "../render/renderer.h"        // for Renderer, which owns the descriptor heap
#include "resource_handle.h"
#include <cstdint>
#include <vector>

namespace scene
{

// =============================================================================
// Resource Handle — 32-bit packed (20 index + 12 generation)
// =============================================================================
// =============================================================================
// MaterialData — CPU-side material definition
// =============================================================================
struct MaterialData
{
    core::Color albedo    = core::Color::White();
    float       roughness = 0.5f;
    float       metallic  = 0.0f;
    char        name[32]  = {};
};

// =============================================================================
// ResourceManager
// =============================================================================
class ResourceManager
{
public:
    void Init();
    void Shutdown(render::D3D12Device& device, render::Renderer& renderer);

    // --- Mesh management ---
    MeshHandle AddMesh(render::Mesh&& mesh, const char* name = nullptr);
    const render::Mesh* GetMesh(MeshHandle handle) const;
    bool IsValidMesh(MeshHandle handle) const;
    void RemoveMesh(MeshHandle handle, render::D3D12Device& device);

    // --- Material management ---
    MaterialHandle AddMaterial(const MaterialData& material);
    const MaterialData* GetMaterial(MaterialHandle handle) const;
    bool IsValidMaterial(MaterialHandle handle) const;
    void RemoveMaterial(MaterialHandle handle);

    // --- Texture management ---
    TextureHandle AddTexture(render::Texture&& texture, const char* name = nullptr);
    const render::Texture* GetTexture(TextureHandle handle) const;
    bool IsValidTexture(TextureHandle handle) const;
    void RemoveTexture(TextureHandle handle, render::D3D12Device& device,
                       render::Renderer& renderer);

    // --- Statistics ---
    uint32_t MeshCount() const { return m_meshAliveCount; }
    uint32_t MaterialCount() const { return m_materialAliveCount; }
    uint32_t TextureCount() const { return m_textureAliveCount; }

private:
    // Mesh pool
    struct MeshSlot
    {
        render::Mesh mesh;
        char name[32] = {};
        uint32_t generation = 0;
        bool alive = false;
    };
    std::vector<MeshSlot> m_meshSlots;
    std::vector<uint32_t> m_meshFreeList;
    uint32_t m_meshAliveCount = 0;

    // Material pool
    struct MaterialSlot
    {
        MaterialData data;
        uint32_t generation = 0;
        bool alive = false;
    };
    std::vector<MaterialSlot> m_materialSlots;
    std::vector<uint32_t> m_materialFreeList;
    uint32_t m_materialAliveCount = 0;

    // Texture pool
    struct TextureSlot
    {
        render::Texture texture;
        char name[32] = {};
        uint32_t generation = 0;
        bool alive = false;
    };
    std::vector<TextureSlot> m_textureSlots;
    std::vector<uint32_t> m_textureFreeList;
    uint32_t m_textureAliveCount = 0;
};

} // namespace scene
