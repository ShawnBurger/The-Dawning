#pragma once
// =============================================================================
// ecs/registry.h — ECS Registry
// =============================================================================
// Central coordinator that owns:
//   - EntityManager (creates/destroys entities)
//   - Type-indexed ComponentPool<T> instances (one per component type)
//
// Usage:
//   Registry reg;
//   Entity e = reg.Create();
//   reg.Assign<Transform>(e, Transform{ ... });
//   reg.Assign<Material>(e, Material{ ... });
//   auto& t = reg.Get<Transform>(e);
//   reg.Destroy(e);
//
// Iteration:
//   auto* pool = reg.GetPool<Transform>();
//   for (uint32_t i = 0; i < pool->Count(); i++) {
//       uint32_t entityIdx = pool->EntityAt(i);
//       auto& transform = pool->DataAt(i);
//   }
// =============================================================================

#include "entity.h"
#include "component_pool.h"
#include <unordered_map>
#include <memory>
#include <typeindex>
#include <cstdint>

namespace ecs
{

class Registry
{
public:
    // --- Entity lifecycle ---
    Entity Create()
    {
        return m_entities.Create();
    }

    void Destroy(Entity entity)
    {
        if (!m_entities.IsAlive(entity)) return;

        // Remove from all pools
        uint32_t idx = entity.Index();
        for (auto& [typeId, pool] : m_pools)
        {
            if (pool->Has(idx))
                pool->Remove(idx);
        }

        m_entities.Destroy(entity);
    }

    bool IsAlive(Entity entity) const
    {
        return m_entities.IsAlive(entity);
    }

    // Recover the current generational handle while iterating a component pool
    // by raw entity index. Returns NullEntity for a dead or unseen slot.
    Entity EntityAtIndex(uint32_t entityIndex) const
    {
        return m_entities.EntityAtIndex(entityIndex);
    }

    uint32_t EntityCount() const { return m_entities.AliveCount(); }

    // --- Component operations ---

    // Every Entity-keyed operation below validates the generation via
    // m_entities.IsAlive(). Without that check a stale handle to a destroyed and
    // recycled slot silently addressed the NEW occupant's components, which
    // defeats the entire point of a 20-bit index + 12-bit generation handle.

    template<typename T>
    T& Assign(Entity entity, const T& component = T{})
    {
        if (!m_entities.IsAlive(entity))
            return FallbackComponent<T>();
        auto& pool = GetOrCreatePool<T>();
        pool.Add(entity.Index(), component);
        return pool.Get(entity.Index());
    }

    template<typename T>
    void Remove(Entity entity)
    {
        if (!m_entities.IsAlive(entity)) return;
        auto* pool = GetPool<T>();
        if (pool) pool->Remove(entity.Index());
    }

    template<typename T>
    bool Has(Entity entity) const
    {
        if (!m_entities.IsAlive(entity)) return false;
        auto* pool = GetPool<T>();
        return pool && pool->Has(entity.Index());
    }

    // Preferred accessor: nullptr when the handle is stale or the component is
    // absent. Get() cannot express that, since it must return a reference.
    template<typename T>
    T* TryGet(Entity entity)
    {
        if (!m_entities.IsAlive(entity)) return nullptr;
        auto* pool = GetPool<T>();
        if (!pool || !pool->Has(entity.Index())) return nullptr;
        return &pool->Get(entity.Index());
    }

    template<typename T>
    const T* TryGet(Entity entity) const
    {
        if (!m_entities.IsAlive(entity)) return nullptr;
        auto* pool = GetPool<T>();
        if (!pool || !pool->Has(entity.Index())) return nullptr;
        return &pool->Get(entity.Index());
    }

    // Returns a shared fallback instance rather than dereferencing null when the
    // handle is stale or the type was never assigned. Previously GetPool<T>()
    // returned nullptr for a never-seen type and this dereferenced it outright.
    // Prefer TryGet() where absence is expected.
    template<typename T>
    T& Get(Entity entity)
    {
        if (T* found = TryGet<T>(entity)) return *found;
        return FallbackComponent<T>();
    }

    template<typename T>
    const T& Get(Entity entity) const
    {
        if (const T* found = TryGet<T>(entity)) return *found;
        return FallbackComponent<T>();
    }

    // Get by raw index (for systems iterating pools). No generation is available
    // here by construction, so this is only safe while iterating a live pool.
    // Guarded against a never-created pool, which previously null-dereferenced.
    template<typename T>
    T& GetByIndex(uint32_t entityIndex)
    {
        auto* pool = GetPool<T>();
        if (!pool || !pool->Has(entityIndex)) return FallbackComponent<T>();
        return pool->Get(entityIndex);
    }

    template<typename T>
    bool HasByIndex(uint32_t entityIndex) const
    {
        auto* pool = GetPool<T>();
        return pool && pool->Has(entityIndex);
    }

    // --- Pool access (for iteration) ---

    template<typename T>
    ComponentPool<T>* GetPool()
    {
        auto it = m_pools.find(std::type_index(typeid(T)));
        if (it == m_pools.end()) return nullptr;
        return static_cast<ComponentPool<T>*>(it->second.get());
    }

    template<typename T>
    const ComponentPool<T>* GetPool() const
    {
        auto it = m_pools.find(std::type_index(typeid(T)));
        if (it == m_pools.end()) return nullptr;
        return static_cast<const ComponentPool<T>*>(it->second.get());
    }

    // Shared per-type sink returned when a component access is invalid (stale
    // handle, never-assigned type, absent component). Reset on every handout so a
    // write through one invalid access cannot be observed by a later one. This
    // trades a hard crash for defined-but-wrong data; use TryGet() when absence is
    // an expected outcome rather than a bug.
    template<typename T>
    static T& FallbackComponent()
    {
        static T fallback{};
        fallback = T{};
        return fallback;
    }

    // --- Utility ---

    // Iterate all entities with BOTH component A and B.
    // Iterates the smaller pool and checks the larger.
    // Callback: void(uint32_t entityIndex, A&, B&)
    template<typename A, typename B, typename Func>
    void Each(Func&& func)
    {
        auto* poolA = GetPool<A>();
        auto* poolB = GetPool<B>();
        if (!poolA || !poolB) return;

        // Iterate the smaller pool for efficiency
        if (poolA->Count() <= poolB->Count())
        {
            for (uint32_t i = 0; i < poolA->Count(); i++)
            {
                uint32_t idx = poolA->EntityAt(i);
                if (poolB->Has(idx))
                    func(idx, poolA->DataAt(i), poolB->Get(idx));
            }
        }
        else
        {
            for (uint32_t i = 0; i < poolB->Count(); i++)
            {
                uint32_t idx = poolB->EntityAt(i);
                if (poolA->Has(idx))
                    func(idx, poolA->Get(idx), poolB->DataAt(i));
            }
        }
    }

    // Iterate all entities with components A, B, AND C.
    //
    // REENTRANCY: the callback receives references directly into the pools' dense
    // arrays. Calling Assign<A/B/C>() from inside the callback can reallocate a
    // dense array (see ComponentPool::EnsureDense) and dangle every reference the
    // callback is holding, including the loop's own. Queue structural changes and
    // apply them after iteration finishes. Spawning during update is the single
    // most common thing a gameplay system does, so this will matter.
    template<typename A, typename B, typename C, typename Func>
    void Each(Func&& func)
    {
        auto* poolA = GetPool<A>();
        auto* poolB = GetPool<B>();
        auto* poolC = GetPool<C>();
        if (!poolA || !poolB || !poolC) return;

        // Always driven from pool A. The two-pool overload above picks the smaller
        // pool, but doing that here would require dispatching over three distinct
        // pool types; the previous code computed a `smallest`/`minCount` pair and
        // then ignored both, which read as an optimization that was not happening.
        for (uint32_t i = 0; i < poolA->Count(); i++)
        {
            uint32_t idx = poolA->EntityAt(i);
            if (poolB->Has(idx) && poolC->Has(idx))
                func(idx, poolA->DataAt(i), poolB->Get(idx), poolC->Get(idx));
        }
    }

private:
    template<typename T>
    ComponentPool<T>& GetOrCreatePool()
    {
        auto key = std::type_index(typeid(T));
        auto it = m_pools.find(key);
        if (it == m_pools.end())
        {
            auto pool = std::make_unique<ComponentPool<T>>();
            auto* ptr = pool.get();
            m_pools[key] = std::move(pool);
            return *ptr;
        }
        return *static_cast<ComponentPool<T>*>(it->second.get());
    }

    EntityManager m_entities;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_pools;
};

} // namespace ecs
