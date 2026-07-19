#pragma once
// =============================================================================
// render/deferred_release.h — fence-guarded release queue
// =============================================================================
// The bookkeeping half of D3D12Device's deferred release, deliberately split out
// with no D3D12 dependency so it can be unit tested without a GPU. The device
// owns the fence; this owns the ordering.
//
// Contract: entries are pushed with the fence value that must COMPLETE before
// the payload may be destroyed. Process(completed) destroys every entry whose
// fence value the GPU has reached, in push order.
// =============================================================================

#include <cstdint>
#include <utility>
#include <vector>

namespace render
{

template <typename T>
class DeferredReleaseQueue
{
public:
    void Push(uint64_t fenceValue, T payload)
    {
        m_entries.push_back(Entry{ fenceValue, std::move(payload) });
    }

    // Destroys every entry with fenceValue <= completed. Returns how many were
    // released, which is what the tests assert on.
    //
    // Callers push in non-decreasing fence order (a frame's fence value never
    // goes backwards), so the survivors are always a suffix and this can stop at
    // the first live entry rather than scanning the whole queue. The loop below
    // does not ASSUME that ordering holds, however: it stops at the first entry
    // it cannot release, so an out-of-order push delays that entry to a later
    // Process call rather than releasing something early. Late is safe; early is
    // a use-after-free.
    size_t Process(uint64_t completed)
    {
        size_t firstLive = 0;
        while (firstLive < m_entries.size() &&
               m_entries[firstLive].fenceValue <= completed)
        {
            ++firstLive;
        }

        if (firstLive > 0)
            m_entries.erase(m_entries.begin(), m_entries.begin() + firstLive);

        return firstLive;
    }

    // Unconditional drain, for shutdown and device-loss. A lost device never
    // advances its fence again, so Process() would leak the queue to process
    // exit; by then nothing can be executing anyway.
    size_t Clear()
    {
        const size_t released = m_entries.size();
        m_entries.clear();
        return released;
    }

    size_t Size()  const { return m_entries.size(); }
    bool   Empty() const { return m_entries.empty(); }

private:
    struct Entry
    {
        uint64_t fenceValue = 0;
        T        payload{};
    };

    std::vector<Entry> m_entries;
};

} // namespace render
