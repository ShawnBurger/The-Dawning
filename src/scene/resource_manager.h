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
// HAZARD — GPU RELEASE IS SYNCHRONOUS, NOT DEFERRED.
//   There is no deferred-release queue and no fence guard in this class.
//   RemoveMesh (resource_manager.cpp:94-107) assigns `slot.mesh = render::Mesh{}`
//   and RemoveTexture (resource_manager.cpp:209-221) assigns
//   `slot.texture = render::Texture{}`. Both drop the last ComPtr reference
//   immediately, on the calling thread, at the moment of the call. If the GPU
//   still references that buffer or texture from a command list that has been
//   recorded but not yet retired — the normal case, since the device runs up to
//   kFrameCount frames in flight — this is a use-after-free. The generational
//   handle does NOT protect against it: it only invalidates future CPU lookups,
//   and the GPU never consults it.
//
//   Do not call RemoveMesh or RemoveTexture mid-frame. The only safe sequence
//   today is a full D3D12Device::WaitForGpu() before the removal.
//   (RemoveMaterial is exempt — MaterialData is CPU-only and holds no GPU
//   objects. RemoveTexture additionally leaks the shader-visible descriptor slot
//   that Renderer::RegisterTexture allocated for it; that allocator has no free
//   list.)
//
//   If you add a real deferred path, put it in D3D12Device (a fence-value-tagged
//   release queue drained at frame start) rather than here, so every subsystem
//   gets it; several other subsystems have the same problem.
// =============================================================================

#include "../render/mesh.h"
#include "../render/texture.h"
#include "../core/types.h"
#include <cstdint>
#include <vector>
#include <queue>   // UNUSED — left over from the deferred-release design that was
                   // never implemented. Nothing in this header or its .cpp uses
                   // std::queue. Safe to delete; see the HAZARD note above.

namespace scene
{

// =============================================================================
// Resource Handle — 32-bit packed (20 index + 12 generation)
// =============================================================================
struct ResourceHandle
{
    uint32_t value = UINT32_MAX;

    static constexpr uint32_t kIndexBits = 20;
    static constexpr uint32_t kGenBits   = 12;
    static constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1;
    static constexpr uint32_t kGenMask   = (1u << kGenBits) - 1;

    ResourceHandle() = default;
    explicit ResourceHandle(uint32_t raw) : value(raw) {}
    ResourceHandle(uint32_t index, uint32_t gen)
        : value((gen << kIndexBits) | (index & kIndexMask)) {}

    uint32_t Index() const      { return value & kIndexMask; }
    uint32_t Generation() const { return (value >> kIndexBits) & kGenMask; }
    bool     IsValid() const    { return value != UINT32_MAX; }
};

using MeshHandle = ResourceHandle;
using MaterialHandle = ResourceHandle;
using TextureHandle = ResourceHandle;

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
    void Shutdown();

    // --- Mesh management ---
    MeshHandle AddMesh(render::Mesh&& mesh, const char* name = nullptr);
    const render::Mesh* GetMesh(MeshHandle handle) const;
    bool IsValidMesh(MeshHandle handle) const;
    void RemoveMesh(MeshHandle handle);

    // --- Material management ---
    MaterialHandle AddMaterial(const MaterialData& material);
    const MaterialData* GetMaterial(MaterialHandle handle) const;
    bool IsValidMaterial(MaterialHandle handle) const;

    // --- Texture management ---
    TextureHandle AddTexture(render::Texture&& texture, const char* name = nullptr);
    const render::Texture* GetTexture(TextureHandle handle) const;
    bool IsValidTexture(TextureHandle handle) const;
    void RemoveTexture(TextureHandle handle);

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
