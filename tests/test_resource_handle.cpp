#include "test_framework.h"
#include "scene/resource_handle.h"
#include "scene/handle_slot_map.h"

#include <type_traits>

static_assert(!std::is_same_v<scene::MeshHandle, scene::MaterialHandle>);
static_assert(!std::is_same_v<scene::MeshHandle, scene::TextureHandle>);
static_assert(!std::is_constructible_v<scene::MeshHandle, scene::TextureHandle>);

TEST_CASE(ResourceHandle_PacksIndexAndGeneration)
{
    const scene::MeshHandle handle(0x54321u, 0xABCu);
    CHECK(handle.IsValid());
    CHECK_EQ(handle.Index(), (uint32_t)0x54321u);
    CHECK_EQ(handle.Generation(), (uint32_t)0xABCu);
    CHECK_FALSE(scene::MeshHandle{}.IsValid());
}

TEST_CASE(ResourceHandle_ConstructorMasksPackedFields)
{
    const scene::MeshHandle handle(0x1ABCDEu, 0x1FFFu);
    CHECK_EQ(handle.Index(), 0x1ABCDEu & scene::MeshHandle::kIndexMask);
    CHECK_EQ(handle.Generation(), 0x1FFFu & scene::MeshHandle::kGenMask);
    CHECK_EQ(scene::MeshHandle::NextGeneration(scene::MeshHandle::kGenMask),
             scene::MeshHandle::kGenMask);
    CHECK_FALSE(scene::MeshHandle::CanRecycleGeneration(scene::MeshHandle::kGenMask));
    CHECK(scene::MeshHandle::CanRecycleGeneration(scene::MeshHandle::kGenMask - 1));
}

TEST_CASE(HandleSlotMap_RejectsRecycledIndexWithStaleGeneration)
{
    scene::HandleSlotMap<scene::MeshHandle, uint32_t> cache;
    const scene::MeshHandle oldMesh(7, 2);
    const scene::MeshHandle replacement(7, 3);

    CHECK(cache.Set(oldMesh, 41));
    uint32_t blasIndex = UINT32_MAX;
    CHECK(cache.TryGet(oldMesh, blasIndex));
    CHECK_EQ(blasIndex, (uint32_t)41);

    CHECK_FALSE(cache.Contains(replacement));
    CHECK(cache.Set(replacement, 99));
    CHECK_FALSE(cache.Contains(oldMesh));
    CHECK(cache.TryGet(replacement, blasIndex));
    CHECK_EQ(blasIndex, (uint32_t)99);
    CHECK_EQ(cache.SlotCount(), (size_t)8);

    cache.Clear();
    CHECK_FALSE(cache.Contains(replacement));
    CHECK_EQ(cache.SlotCount(), (size_t)0);
}

TEST_CASE(HandleSlotMap_InvalidAndUnseenHandlesMissWithoutGrowing)
{
    scene::HandleSlotMap<scene::TextureHandle, uint32_t> cache;
    uint32_t value = 0;

    CHECK_FALSE(cache.Set({}, 4));
    CHECK_FALSE(cache.TryGet({}, value));
    CHECK_FALSE(cache.TryGet(scene::TextureHandle(12, 1), value));
    CHECK_EQ(cache.SlotCount(), (size_t)0);
}
