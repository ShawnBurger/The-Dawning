#pragma once

#include <cstdint>

namespace scene
{

// Packed generational handles share a representation but remain distinct C++
// types. Passing a TextureHandle to GetMesh is therefore a compile error.
template <typename Tag>
struct ResourceHandle
{
    uint32_t value = UINT32_MAX;

    static constexpr uint32_t kIndexBits = 20;
    static constexpr uint32_t kGenBits   = 12;
    static constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1;
    static constexpr uint32_t kGenMask   = (1u << kGenBits) - 1;

    ResourceHandle() = default;
    explicit ResourceHandle(uint32_t raw) : value(raw) {}
    ResourceHandle(uint32_t index, uint32_t generation)
        : value(((generation & kGenMask) << kIndexBits) | (index & kIndexMask)) {}

    uint32_t Index() const      { return value & kIndexMask; }
    uint32_t Generation() const { return (value >> kIndexBits) & kGenMask; }
    bool IsValid() const        { return value != UINT32_MAX; }

    static uint32_t NextGeneration(uint32_t generation)
    {
        return generation < kGenMask ? generation + 1u : kGenMask;
    }

    static bool CanRecycleGeneration(uint32_t generation) { return generation < kGenMask; }

    bool operator==(const ResourceHandle& other) const { return value == other.value; }
    bool operator!=(const ResourceHandle& other) const { return value != other.value; }
};

struct MeshResourceTag;
struct MaterialResourceTag;
struct TextureResourceTag;

using MeshHandle     = ResourceHandle<MeshResourceTag>;
using MaterialHandle = ResourceHandle<MaterialResourceTag>;
using TextureHandle  = ResourceHandle<TextureResourceTag>;

} // namespace scene
