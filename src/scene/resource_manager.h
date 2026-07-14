#pragma once
// =============================================================================
// scene/resource_manager.h — Handle-Based Resource Management
// =============================================================================
// Manages GPU resources (meshes, materials) via generational handles.
// ECS components store handles, never raw pointers or GPU addresses.
//
// Features:
//   - Slot-map pools with O(1) lookup by handle
//   - Generational handles prevent use-after-free
//   - Deferred GPU resource release via fence-guarded queue
//   - Multiple entities can share the same mesh/material
// =============================================================================

#include "../render/mesh.h"
#include "../core/types.h"
#include <cstdint>
#include <vector>
#include <queue>

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

    // --- Statistics ---
    uint32_t MeshCount() const { return m_meshAliveCount; }
    uint32_t MaterialCount() const { return m_materialAliveCount; }

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
};

} // namespace scene
