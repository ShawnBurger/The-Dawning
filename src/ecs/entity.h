#pragma once
// =============================================================================
// ecs/entity.h — Generational Entity ID
// =============================================================================
// 32-bit packed entity handle: 20-bit index + 12-bit generation.
// - Index provides O(1) lookup into sparse arrays
// - Generation prevents ABA problem when slots are recycled
// - Max ~1M concurrent entities, 4096 generations per slot
//
// The all-ones packed value is reserved for NullEntity. EntityManager therefore
// allocates indices [0, kMaxIndex), keeping every allocated slot valid through
// all 4096 generation values.
//
// Based on EnTT's entity model (used in Minecraft Bedrock Edition).
// =============================================================================

#include <cstdint>

namespace ecs
{

struct Entity
{
    uint32_t id = UINT32_MAX;

    static constexpr uint32_t kIndexBits = 20;
    static constexpr uint32_t kGenBits   = 12;
    static constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1;    // 0xFFFFF
    static constexpr uint32_t kGenMask   = (1u << kGenBits) - 1;      // 0xFFF
    static constexpr uint32_t kMaxIndex  = kIndexMask;                 // 1,048,575
    static constexpr uint32_t kMaxGen    = kGenMask;                   // 4,095
    static constexpr uint32_t kNull      = UINT32_MAX;

    Entity() = default;
    explicit Entity(uint32_t rawId) : id(rawId) {}
    Entity(uint32_t index, uint32_t generation)
        : id((generation << kIndexBits) | (index & kIndexMask)) {}

    uint32_t Index() const      { return id & kIndexMask; }
    uint32_t Generation() const { return (id >> kIndexBits) & kGenMask; }
    bool     IsNull() const     { return id == kNull; }
    bool     IsValid() const    { return id != kNull; }

    bool operator==(const Entity& other) const { return id == other.id; }
    bool operator!=(const Entity& other) const { return id != other.id; }
    bool operator<(const Entity& other) const  { return id < other.id; }
};

static constexpr Entity NullEntity = Entity();

// =============================================================================
// EntityManager — allocates and recycles entity slots
// =============================================================================
class EntityManager
{
public:
    Entity Create()
    {
        if (m_freeHead != UINT32_MAX)
        {
            // Recycle from free list
            uint32_t index = m_freeHead;
            m_freeHead = m_slots[index].nextFree;
            m_slots[index].alive = true;
            m_aliveCount++;
            return Entity(index, m_slots[index].generation);
        }

        // Allocate new slot
        if (m_slotCount >= Entity::kMaxIndex)
            return NullEntity; // Out of entity slots

        // Grow BEFORE bumping m_slotCount. With the post-increment first, Grow()'s
        // copy loop ran to m_slotCount == index + 1 while the old array held only
        // m_capacity == index elements, reading one full Slot past the allocation.
        uint32_t index = m_slotCount;
        if (index >= m_capacity)
            Grow();
        m_slotCount++;

        m_slots[index].generation = 0;
        m_slots[index].alive = true;
        m_slots[index].nextFree = UINT32_MAX;
        m_aliveCount++;
        return Entity(index, 0);
    }

    void Destroy(Entity entity)
    {
        uint32_t index = entity.Index();
        if (index >= m_slotCount) return;
        auto& slot = m_slots[index];
        if (!slot.alive || slot.generation != entity.Generation()) return;

        slot.alive = false;

        // Wrapping to generation zero would make the oldest stale handle for
        // this index valid again. Retire terminal-generation slots permanently
        // instead. Slot exhaustion after 4096 complete lifetimes is preferable
        // to an ABA alias that can mutate an unrelated live entity.
        if (slot.generation == Entity::kMaxGen)
        {
            slot.nextFree = UINT32_MAX;
        }
        else
        {
            slot.generation++;
            slot.nextFree = m_freeHead;
            m_freeHead = index;
        }
        m_aliveCount--;
    }

    bool IsAlive(Entity entity) const
    {
        uint32_t index = entity.Index();
        if (index >= m_slotCount) return false;
        return m_slots[index].alive && m_slots[index].generation == entity.Generation();
    }

    Entity EntityAtIndex(uint32_t index) const
    {
        if (index >= m_slotCount || !m_slots[index].alive)
            return NullEntity;
        return Entity(index, m_slots[index].generation);
    }

    uint32_t AliveCount() const { return m_aliveCount; }
    uint32_t SlotCount() const  { return m_slotCount; }

    ~EntityManager() { delete[] m_slots; }

    // Non-copyable
    EntityManager() = default;
    EntityManager(const EntityManager&) = delete;
    EntityManager& operator=(const EntityManager&) = delete;

private:
    void Grow()
    {
        uint32_t newCap = m_capacity == 0 ? 256 : m_capacity * 2;
        if (newCap > Entity::kMaxIndex + 1) newCap = Entity::kMaxIndex + 1;

        auto* newSlots = new Slot[newCap];
        if (m_slots)
        {
            for (uint32_t i = 0; i < m_slotCount; i++)
                newSlots[i] = m_slots[i];
            delete[] m_slots;
        }
        m_slots = newSlots;
        m_capacity = newCap;
    }

    struct Slot
    {
        uint32_t generation = 0;
        uint32_t nextFree = UINT32_MAX;
        bool alive = false;
    };

    Slot*    m_slots = nullptr;
    uint32_t m_capacity = 0;
    uint32_t m_slotCount = 0;
    uint32_t m_aliveCount = 0;
    uint32_t m_freeHead = UINT32_MAX;
};

} // namespace ecs
