// =============================================================================
// scene/resource_manager.cpp — Resource Manager Implementation
// =============================================================================

#include "resource_manager.h"
#include "../core/log.h"
#include <cstring>
#include <cstdint>

namespace scene
{

void ResourceManager::Init()
{
    m_meshSlots.reserve(64);
    m_materialSlots.reserve(64);
    m_textureSlots.reserve(64);
    core::Log::Info("ResourceManager initialized");
}

void ResourceManager::Shutdown(render::D3D12Device& device, render::Renderer& renderer)
{
    for (uint32_t i = 0; i < m_meshSlots.size(); ++i)
    {
        if (m_meshSlots[i].alive)
            RemoveMesh(MeshHandle(i, m_meshSlots[i].generation), device);
    }
    m_meshSlots.clear();
    m_meshFreeList.clear();
    m_meshAliveCount = 0;

    m_materialSlots.clear();
    m_materialFreeList.clear();
    m_materialAliveCount = 0;

    for (uint32_t i = 0; i < m_textureSlots.size(); ++i)
    {
        if (m_textureSlots[i].alive)
        {
            RemoveTexture(TextureHandle(i, m_textureSlots[i].generation),
                          device, renderer);
        }
    }
    m_textureSlots.clear();
    m_textureFreeList.clear();
    m_textureAliveCount = 0;

    core::Log::Info("ResourceManager shut down");
}

// =============================================================================
// Mesh Pool
// =============================================================================
MeshHandle ResourceManager::AddMesh(render::Mesh&& mesh, const char* name)
{
    if (!mesh.IsValid())
    {
        core::Log::Error("Refusing to register an invalid mesh");
        return {};
    }

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
        if (m_meshSlots.size() > MeshHandle::kIndexMask)
        {
            core::Log::Error("Mesh handle index space exhausted");
            return {};
        }
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

void ResourceManager::RemoveMesh(MeshHandle handle, render::D3D12Device& device)
{
    if (!IsValidMesh(handle)) return;
    uint32_t idx = handle.Index();
    auto& slot = m_meshSlots[idx];

    const bool canRecycle = MeshHandle::CanRecycleGeneration(slot.generation);
    slot.alive = false;
    slot.generation = MeshHandle::NextGeneration(slot.generation);

    // Hand the GPU buffers to the device's fence-guarded queue rather than
    // dropping them here. Overwriting the slot releases the last reference
    // immediately, and up to kFrameCount frames may still have command lists
    // referencing these buffers - that is a use-after-free, not a leak.
    device.DeferredRelease(slot.mesh.vertexBuffer);
    device.DeferredRelease(slot.mesh.indexBuffer);
    slot.mesh = render::Mesh{};

    // Retire a slot permanently when its packed generation is exhausted. This
    // prevents an ancient generation-zero handle becoming valid again via wrap.
    if (canRecycle)
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
        if (m_materialSlots.size() > MaterialHandle::kIndexMask)
        {
            core::Log::Error("Material handle index space exhausted");
            return {};
        }
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

void ResourceManager::RemoveMaterial(MaterialHandle handle)
{
    if (!IsValidMaterial(handle)) return;
    const uint32_t idx = handle.Index();
    MaterialSlot& slot = m_materialSlots[idx];
    const bool canRecycle =
        MaterialHandle::CanRecycleGeneration(slot.generation);
    slot.alive = false;
    slot.generation = MaterialHandle::NextGeneration(slot.generation);
    slot.data = {};
    if (canRecycle)
        m_materialFreeList.push_back(idx);
    --m_materialAliveCount;
}

// =============================================================================
// Texture Pool
// =============================================================================
TextureHandle ResourceManager::AddTexture(render::Texture&& texture, const char* name)
{
    if (!texture.IsValid())
    {
        core::Log::Error("Refusing to register an invalid texture");
        return {};
    }

    uint32_t index;
    uint32_t gen;
    bool reused = false;

    if (!m_textureFreeList.empty())
    {
        index = m_textureFreeList.back();
        m_textureFreeList.pop_back();
        gen = m_textureSlots[index].generation;
        reused = true;
    }
    else
    {
        if (m_textureSlots.size() > TextureHandle::kIndexMask)
        {
            core::Log::Error("Texture handle index space exhausted");
            return {};
        }
        index = static_cast<uint32_t>(m_textureSlots.size());
        m_textureSlots.emplace_back();
        gen = 0;
    }

    auto& slot = m_textureSlots[index];
    if (!slot.texture.Adopt(std::move(texture)))
    {
        core::Log::Errorf("Texture slot %u still owns an unretired resource", index);
        if (reused)
            m_textureFreeList.push_back(index);
        else
            m_textureSlots.pop_back();
        return TextureHandle{};
    }
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

    m_textureAliveCount++;
    core::Log::Infof("Texture added: handle=%u gen=%u name='%s'", index, gen, slot.name);
    return TextureHandle(index, gen);
}

const render::Texture* ResourceManager::GetTexture(TextureHandle handle) const
{
    if (!IsValidTexture(handle)) return nullptr;
    return &m_textureSlots[handle.Index()].texture;
}

bool ResourceManager::IsValidTexture(TextureHandle handle) const
{
    if (!handle.IsValid()) return false;
    uint32_t idx = handle.Index();
    if (idx >= m_textureSlots.size()) return false;
    return m_textureSlots[idx].alive && m_textureSlots[idx].generation == handle.Generation();
}

void ResourceManager::RemoveTexture(TextureHandle handle, render::D3D12Device& device,
                                    render::Renderer& renderer)
{
    if (!IsValidTexture(handle)) return;
    uint32_t idx = handle.Index();
    auto& slot = m_textureSlots[idx];

    const bool canRecycle = TextureHandle::CanRecycleGeneration(slot.generation);
    slot.alive = false;
    slot.generation = TextureHandle::NextGeneration(slot.generation);

    // Two separate lifetimes, both fence-guarded. The RESOURCE goes to the
    // device's deferred queue; the DESCRIPTOR that names it goes back to the
    // renderer's allocator. Releasing either one eagerly is a use-after-free
    // while frames are still in flight - the descriptor case is the subtler of
    // the two, because the GPU reads shader-visible descriptors at execution
    // time, so a recycled slot would silently point a recorded draw at a
    // different texture.
    device.DeferredRelease(slot.texture.resource);
    renderer.ReleaseTextureDescriptor(device, slot.texture.descriptor);
    slot.texture.ResetAfterRetirement();

    if (canRecycle)
        m_textureFreeList.push_back(idx);
    m_textureAliveCount--;
}

} // namespace scene
