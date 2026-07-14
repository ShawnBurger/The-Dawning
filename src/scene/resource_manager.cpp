// =============================================================================
// scene/resource_manager.cpp — Resource Manager Implementation
// =============================================================================

#include "resource_manager.h"
#include "../core/log.h"
#include <cstring>

namespace scene
{

void ResourceManager::Init()
{
    m_meshSlots.reserve(64);
    m_materialSlots.reserve(64);
    core::Log::Info("ResourceManager initialized");
}

void ResourceManager::Shutdown()
{
    // GPU resources in mesh slots are released via ComPtr destructors
    m_meshSlots.clear();
    m_meshFreeList.clear();
    m_meshAliveCount = 0;

    m_materialSlots.clear();
    m_materialFreeList.clear();
    m_materialAliveCount = 0;

    core::Log::Info("ResourceManager shut down");
}

// =============================================================================
// Mesh Pool
// =============================================================================
MeshHandle ResourceManager::AddMesh(render::Mesh&& mesh, const char* name)
{
    uint32_t index;
    uint32_t gen;

    if (!m_meshFreeList.empty())
    {
        index = m_meshFreeList.back();
        m_meshFreeList.pop_back();
        gen = m_meshSlots[index].generation;
    }
    else
    {
        index = static_cast<uint32_t>(m_meshSlots.size());
        m_meshSlots.emplace_back();
        gen = 0;
    }

    auto& slot = m_meshSlots[index];
    slot.mesh = std::move(mesh);
    slot.generation = gen;
    slot.alive = true;

    if (name)
    {
        int i = 0;
        while (name[i] && i < 31) { slot.name[i] = name[i]; i++; }
        slot.name[i] = '\0';
    }
    else
    {
        slot.name[0] = '\0';
    }

    m_meshAliveCount++;
    core::Log::Infof("Mesh added: handle=%u gen=%u name='%s'", index, gen, slot.name);
    return MeshHandle(index, gen);
}

const render::Mesh* ResourceManager::GetMesh(MeshHandle handle) const
{
    if (!IsValidMesh(handle)) return nullptr;
    return &m_meshSlots[handle.Index()].mesh;
}

bool ResourceManager::IsValidMesh(MeshHandle handle) const
{
    if (!handle.IsValid()) return false;
    uint32_t idx = handle.Index();
    if (idx >= m_meshSlots.size()) return false;
    return m_meshSlots[idx].alive && m_meshSlots[idx].generation == handle.Generation();
}

void ResourceManager::RemoveMesh(MeshHandle handle)
{
    if (!IsValidMesh(handle)) return;
    uint32_t idx = handle.Index();
    auto& slot = m_meshSlots[idx];

    slot.alive = false;
    slot.generation++;
    // GPU resources released when Mesh's ComPtrs go out of scope or are overwritten
    slot.mesh = render::Mesh{}; // Release GPU refs

    m_meshFreeList.push_back(idx);
    m_meshAliveCount--;
}

// =============================================================================
// Material Pool
// =============================================================================
MaterialHandle ResourceManager::AddMaterial(const MaterialData& material)
{
    uint32_t index;
    uint32_t gen;

    if (!m_materialFreeList.empty())
    {
        index = m_materialFreeList.back();
        m_materialFreeList.pop_back();
        gen = m_materialSlots[index].generation;
    }
    else
    {
        index = static_cast<uint32_t>(m_materialSlots.size());
        m_materialSlots.emplace_back();
        gen = 0;
    }

    auto& slot = m_materialSlots[index];
    slot.data = material;
    slot.generation = gen;
    slot.alive = true;

    m_materialAliveCount++;
    return MaterialHandle(index, gen);
}

const MaterialData* ResourceManager::GetMaterial(MaterialHandle handle) const
{
    if (!IsValidMaterial(handle)) return nullptr;
    return &m_materialSlots[handle.Index()].data;
}

bool ResourceManager::IsValidMaterial(MaterialHandle handle) const
{
    if (!handle.IsValid()) return false;
    uint32_t idx = handle.Index();
    if (idx >= m_materialSlots.size()) return false;
    return m_materialSlots[idx].alive && m_materialSlots[idx].generation == handle.Generation();
}

} // namespace scene
