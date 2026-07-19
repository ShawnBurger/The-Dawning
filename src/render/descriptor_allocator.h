#pragma once
// =============================================================================
// render/descriptor_allocator.h — index allocator for a shader-visible heap
// =============================================================================
// Hands out and RECLAIMS indices into a fixed-size descriptor heap. Deliberately
// has no D3D12 dependency: it allocates integers, the caller turns an integer
// into a descriptor handle. That split makes the part that is easy to get wrong
// — the reuse timing — unit testable without a GPU, the same way
// render/deferred_release.h splits ordering from the fence.
//
// It replaces a bare `m_nextTextureDescriptor++` with no free list, which meant
// every released texture permanently consumed a slot out of 127 usable ones.
//
// THE HAZARD THIS EXISTS TO PREVENT
// Reusing a descriptor slot is not like freeing memory. A command list that has
// been recorded but not yet retired may still reference the slot, and the GPU
// reads shader-visible descriptors at EXECUTION time, not at record time, for
// the volatile descriptor ranges this engine uses. Overwriting a slot while such
// a command list is in flight makes it sample a different resource than the one
// it was recorded against. So Release() does not return the index to the free
// list; it parks it against the fence value that must complete first, and
// Reclaim() moves it across once the GPU has passed that value.
//
// Late reuse is a wasted slot for a frame or two. Early reuse is a corrupted
// frame. The asymmetry is why the pending queue exists.
// =============================================================================

#include <cstdint>
#include <vector>

namespace render
{

class DescriptorAllocator
{
public:
    static constexpr uint32_t kInvalid = UINT32_MAX;

    // `firstIndex` reserves a prefix the allocator will never hand out — the
    // raster heap keeps index 0 as a permanent null-SRV fallback.
    void Init(uint32_t capacity, uint32_t firstIndex = 0)
    {
        m_capacity   = capacity;
        m_firstIndex = firstIndex < capacity ? firstIndex : capacity;
        m_highWater  = m_firstIndex;
        m_inUse      = 0;
        m_free.clear();
        m_pending.clear();
        m_states.assign(capacity, SlotState::NeverAllocated);
        for (uint32_t i = 0; i < m_firstIndex; ++i)
            m_states[i] = SlotState::Reserved;
    }

    // Prefers recycled slots over fresh ones so the high-water mark stays low
    // and heap occupancy is legible when debugging. Returns kInvalid when full.
    uint32_t Allocate()
    {
        if (!m_free.empty())
        {
            const uint32_t index = m_free.back();
            m_free.pop_back();
            m_states[index] = SlotState::InUse;
            ++m_inUse;
            return index;
        }
        if (m_highWater < m_capacity)
        {
            const uint32_t index = m_highWater++;
            m_states[index] = SlotState::InUse;
            ++m_inUse;
            return index;
        }
        return kInvalid;
    }

    // Park `index` until the GPU passes `fenceValue`. Not returned to the free
    // list here — see the header comment.
    bool Release(uint32_t index, uint64_t fenceValue)
    {
        if (index == kInvalid || index < m_firstIndex || index >= m_capacity)
            return false;
        if (m_states[index] != SlotState::InUse)
            return false;

        m_states[index] = SlotState::Pending;
        --m_inUse;
        m_pending.push_back(Pending{ fenceValue, index });
        return true;
    }

    // Move every slot the GPU has finished with into the free list. Returns how
    // many were reclaimed. Does not assume the pending queue is fence-ordered:
    // it scans the whole queue and keeps what is not ready, so an out-of-order
    // Release delays a slot rather than releasing it early.
    size_t Reclaim(uint64_t completedFenceValue)
    {
        size_t reclaimed = 0;
        size_t keep = 0;
        for (size_t i = 0; i < m_pending.size(); ++i)
        {
            if (m_pending[i].fenceValue <= completedFenceValue)
            {
                const uint32_t index = m_pending[i].index;
                if (m_states[index] == SlotState::Pending)
                {
                    m_states[index] = SlotState::Free;
                    m_free.push_back(index);
                    ++reclaimed;
                }
            }
            else
            {
                m_pending[keep++] = m_pending[i];
            }
        }
        m_pending.resize(keep);
        return reclaimed;
    }

    // Shutdown / device-loss path: a lost device never advances its fence, so
    // waiting would strand every pending slot.
    void ReclaimAll()
    {
        for (const auto& p : m_pending)
        {
            if (m_states[p.index] == SlotState::Pending)
            {
                m_states[p.index] = SlotState::Free;
                m_free.push_back(p.index);
            }
        }
        m_pending.clear();
    }

    uint32_t Capacity()      const { return m_capacity; }
    uint32_t FirstIndex()    const { return m_firstIndex; }
    uint32_t HighWater()     const { return m_highWater; }
    size_t   FreeCount()     const { return m_free.size(); }
    size_t   PendingCount()  const { return m_pending.size(); }

    uint32_t InUse() const { return m_inUse; }

private:
    enum class SlotState : uint8_t
    {
        Reserved,
        NeverAllocated,
        InUse,
        Pending,
        Free,
    };

    struct Pending
    {
        uint64_t fenceValue = 0;
        uint32_t index      = 0;
    };

    uint32_t m_capacity   = 0;
    uint32_t m_firstIndex = 0;
    uint32_t m_highWater  = 0;
    uint32_t m_inUse      = 0;
    std::vector<uint32_t> m_free;
    std::vector<Pending>  m_pending;
    std::vector<SlotState> m_states;
};

} // namespace render
