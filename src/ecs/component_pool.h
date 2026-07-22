#pragma once
// =============================================================================
// ecs/component_pool.h — Sparse-Set Component Storage
// =============================================================================
// Each component type T gets its own ComponentPool<T> containing:
//   - sparse[]: entity index → dense index (or UINT32_MAX if absent)
//   - dense[]:  packed entity indices (for iteration)
//   - data[]:   packed component data (parallel with dense[])
//
// Operations:
//   Add:    O(1) — append to dense + data, set sparse entry
//   Remove: O(1) — swap-and-pop in dense + data, update sparse
//   Get:    O(1) — sparse[index] → dense index → data[dense]
//   Has:    O(1) — sparse[index] < count && dense[sparse[index]] == index
//   Iterate: O(n) linear scan of dense[] / data[]
//
// Based on EnTT's sparse set model. Swap-and-pop gives cache-friendly
// iteration with no gaps in the dense arrays.
// =============================================================================

#include "entity.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace ecs
{

// =============================================================================
// IComponentPool — type-erased base for registry to store heterogeneous pools
// =============================================================================
class IComponentPool
{
public:
    virtual ~IComponentPool() = default;
    virtual void Remove(uint32_t entityIndex) = 0;
    virtual bool Has(uint32_t entityIndex) const = 0;
    virtual uint32_t Count() const = 0;
};

// =============================================================================
// ComponentPool<T> — typed sparse-set storage
// =============================================================================
template<typename T>
class ComponentPool : public IComponentPool
{
public:
    ComponentPool() = default;
    ~ComponentPool() override
    {
        delete[] m_sparse;
        delete[] m_dense;
        delete[] m_data;
    }

    // Non-copyable
    ComponentPool(const ComponentPool&) = delete;
    ComponentPool& operator=(const ComponentPool&) = delete;

    void Add(uint32_t entityIndex, const T& component)
    {
        // EntityManager deliberately reserves kMaxIndex so the all-ones packed
        // handle remains NullEntity. Reject direct pool calls outside that same
        // domain before capacity doubling can wrap or attempt an enormous alloc.
        if (entityIndex >= Entity::kMaxIndex) return;
        if (Has(entityIndex)) { Get(entityIndex) = component; return; }
        EnsureSparse(entityIndex);
        EnsureDense(m_count + 1);

        m_sparse[entityIndex] = m_count;
        m_dense[m_count] = entityIndex;
        m_data[m_count] = component;
        m_count++;
    }

    void Remove(uint32_t entityIndex) override
    {
        if (!Has(entityIndex)) return;

        uint32_t denseIdx = m_sparse[entityIndex];
        uint32_t lastIdx = m_count - 1;

        if (denseIdx != lastIdx)
        {
            // Swap with last element
            uint32_t lastEntity = m_dense[lastIdx];
            m_dense[denseIdx] = lastEntity;
            m_data[denseIdx] = m_data[lastIdx];
            m_sparse[lastEntity] = denseIdx;
        }

        m_sparse[entityIndex] = UINT32_MAX;
        m_count--;
    }

    bool Has(uint32_t entityIndex) const override
    {
        if (entityIndex >= m_sparseCapacity) return false;
        uint32_t denseIdx = m_sparse[entityIndex];
        return denseIdx < m_count && m_dense[denseIdx] == entityIndex;
    }

    T& Get(uint32_t entityIndex)
    {
        return m_data[m_sparse[entityIndex]];
    }

    const T& Get(uint32_t entityIndex) const
    {
        return m_data[m_sparse[entityIndex]];
    }

    uint32_t Count() const override { return m_count; }

    // Iteration — access packed arrays directly
    // Usage: for (uint32_t i = 0; i < pool.Count(); i++) {
    //            uint32_t entityIdx = pool.EntityAt(i);
    //            T& comp = pool.DataAt(i);
    //        }
    uint32_t EntityAt(uint32_t denseIndex) const { return m_dense[denseIndex]; }
    T&       DataAt(uint32_t denseIndex)         { return m_data[denseIndex]; }
    const T& DataAt(uint32_t denseIndex) const   { return m_data[denseIndex]; }

    // Raw array access for systems that need pointer-based iteration
    const uint32_t* DenseArray() const { return m_dense; }
    T*              DataArray()        { return m_data; }
    const T*        DataArray() const  { return m_data; }

private:
    void EnsureSparse(uint32_t entityIndex)
    {
        if (entityIndex < m_sparseCapacity) return;

        const uint32_t required = entityIndex + 1;
        uint32_t newCap = NextCapacity(m_sparseCapacity, 256, required);
        if (newCap < required || newCap < m_sparseCapacity)
            throw std::length_error("component sparse-set capacity overflow");

        auto newSparse = std::make_unique<uint32_t[]>(newCap);
        std::fill_n(newSparse.get(), newCap, UINT32_MAX);

        if (m_sparse)
        {
            std::copy_n(m_sparse, m_sparseCapacity, newSparse.get());
            delete[] m_sparse;
        }
        m_sparse = newSparse.release();
        m_sparseCapacity = newCap;
    }

    void EnsureDense(uint32_t required)
    {
        if (required <= m_denseCapacity) return;

        uint32_t newCap = NextCapacity(m_denseCapacity, 64, required);
        if (newCap < required || newCap < m_count)
            throw std::length_error("component dense-set capacity overflow");

        auto newDense = std::make_unique<uint32_t[]>(newCap);
        auto newData  = std::make_unique<T[]>(newCap);

        if (m_dense)
        {
            std::copy_n(m_dense, m_count, newDense.get());
            std::copy_n(m_data, m_count, newData.get());
            delete[] m_dense;
            delete[] m_data;
        }
        m_dense = newDense.release();
        m_data = newData.release();
        m_denseCapacity = newCap;
    }

    static uint32_t NextCapacity(uint32_t current, uint32_t initial, uint32_t required)
    {
        uint32_t capacity = current == 0 ? initial : current;
        while (capacity < required)
        {
            if (capacity > Entity::kMaxIndex / 2)
                return Entity::kMaxIndex;
            capacity *= 2;
        }
        return capacity;
    }

    uint32_t* m_sparse = nullptr;
    uint32_t  m_sparseCapacity = 0;
    uint32_t* m_dense = nullptr;
    T*        m_data = nullptr;
    uint32_t  m_denseCapacity = 0;
    uint32_t  m_count = 0;
};

} // namespace ecs
