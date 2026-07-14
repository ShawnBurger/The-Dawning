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

    uint32_t EntityCount() const { return m_entities.AliveCount(); }

    // --- Component operations ---

    template<typename T>
    T& Assign(Entity entity, const T& component = T{})
    {
        auto& pool = GetOrCreatePool<T>();
        pool.Add(entity.Index(), component);
        return pool.Get(entity.Index());
    }

    template<typename T>
    void Remove(Entity entity)
    {
        auto* pool = GetPool<T>();
        if (pool) pool->Remove(entity.Index());
    }

    template<typename T>
    bool Has(Entity entity) const
    {
        auto* pool = GetPool<T>();
        return pool && pool->Has(entity.Index());
    }

    template<typename T>
    T& Get(Entity entity)
    {
        return GetPool<T>()->Get(entity.Index());
    }

    template<typename T>
    const T& Get(Entity entity) const
    {
        return GetPool<T>()->Get(entity.Index());
    }

    // Get by raw index (for systems iterating pools)
    template<typename T>
    T& GetByIndex(uint32_t entityIndex)
    {
        return GetPool<T>()->Get(entityIndex);
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
    template<typename A, typename B, typename C, typename Func>
    void Each(Func&& func)
    {
        auto* poolA = GetPool<A>();
        auto* poolB = GetPool<B>();
        auto* poolC = GetPool<C>();
        if (!poolA || !poolB || !poolC) return;

        // Find smallest pool
        ComponentPool<A>* smallest = poolA;
        uint32_t minCount = poolA->Count();
        if (poolB->Count() < minCount) minCount = poolB->Count();
        if (poolC->Count() < minCount) minCount = poolC->Count();

        // Iterate smallest
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
