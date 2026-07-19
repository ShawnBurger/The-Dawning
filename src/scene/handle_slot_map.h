#pragma once

#include <cstddef>
#include <vector>

namespace scene
{

// A compact cache indexed by a generational handle's slot. The full handle is
// stored in each entry, so recycling an index invalidates the old association.
template <typename Handle, typename Value>
class HandleSlotMap
{
public:
    bool TryGet(Handle handle, Value& outValue) const
    {
        if (!handle.IsValid() || handle.Index() >= m_entries.size())
            return false;

        const Entry& entry = m_entries[handle.Index()];
        if (!entry.handle.IsValid() || entry.handle != handle)
            return false;

        outValue = entry.value;
        return true;
    }

    bool Contains(Handle handle) const
    {
        return handle.IsValid() && handle.Index() < m_entries.size() &&
               m_entries[handle.Index()].handle == handle;
    }

    bool Set(Handle handle, const Value& value)
    {
        if (!handle.IsValid())
            return false;

        if (handle.Index() >= m_entries.size())
            m_entries.resize(static_cast<size_t>(handle.Index()) + 1);

        m_entries[handle.Index()] = Entry{ handle, value };
        return true;
    }

    void Clear() { m_entries.clear(); }
    size_t SlotCount() const { return m_entries.size(); }

private:
    struct Entry
    {
        Handle handle;
        Value value{};
    };

    std::vector<Entry> m_entries;
};

} // namespace scene
