// =============================================================================
// tests/test_descriptor_allocator.cpp — shader-visible heap index allocator
// =============================================================================
// Covers render/descriptor_allocator.h, which has no D3D12 dependency precisely
// so the reuse-timing rules can be tested without a GPU.
//
// The rule under test is asymmetric: handing a slot back LATE wastes a
// descriptor for a frame or two, handing it back EARLY corrupts a frame that is
// still in flight. Every test below is written from that asymmetry.
// =============================================================================

#include "test_framework.h"
#include "render/descriptor_allocator.h"
#include "render/texture.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<render::Texture>);
static_assert(!std::is_copy_assignable_v<render::Texture>);
static_assert(!std::is_move_assignable_v<render::Texture>);

TEST_CASE(Texture_MoveTransfersAndClearsDescriptorOwnership)
{
    render::Texture source;
    source.width = 64;
    source.height = 32;
    source.mipCount = 4;
    source.descriptorIndex = 7;

    render::Texture destination(std::move(source));

    CHECK_EQ(destination.width, (uint32_t)64);
    CHECK_EQ(destination.height, (uint32_t)32);
    CHECK_EQ(destination.mipCount, (uint32_t)4);
    CHECK_EQ(destination.descriptorIndex, (uint32_t)7);
    CHECK_EQ(source.descriptorIndex, UINT32_MAX);
}

TEST_CASE(Texture_AdoptRefusesToOverwriteExistingOwnership)
{
    render::Texture destination;
    destination.descriptorIndex = 3;

    render::Texture source;
    source.descriptorIndex = 7;

    CHECK_FALSE(destination.Adopt(std::move(source)));
    CHECK_EQ(destination.descriptorIndex, (uint32_t)3);
    CHECK_EQ(source.descriptorIndex, (uint32_t)7);

    destination.ResetAfterRetirement();
    CHECK(destination.Adopt(std::move(source)));
    CHECK_EQ(destination.descriptorIndex, (uint32_t)7);
    CHECK_EQ(source.descriptorIndex, UINT32_MAX);
}

TEST_CASE(DescriptorAllocator_ReservesPrefix)
{
    render::DescriptorAllocator a;
    a.Init(128, 1);   // raster heap: slot 0 is the permanent null-SRV fallback

    // First handout must skip the reserved prefix.
    CHECK_EQ(a.Allocate(), (uint32_t)1);
    CHECK_EQ(a.Allocate(), (uint32_t)2);
    CHECK_EQ(a.FirstIndex(), (uint32_t)1);
}

TEST_CASE(DescriptorAllocator_ExhaustsAtCapacity)
{
    render::DescriptorAllocator a;
    a.Init(4, 1);   // usable slots are 1,2,3 — capacity counts the reserved prefix

    CHECK_EQ(a.Allocate(), (uint32_t)1);
    CHECK_EQ(a.Allocate(), (uint32_t)2);
    CHECK_EQ(a.Allocate(), (uint32_t)3);
    CHECK_EQ(a.Allocate(), render::DescriptorAllocator::kInvalid);
    // Exhaustion must be reported, not wrapped around onto live descriptors.
    CHECK_EQ(a.Allocate(), render::DescriptorAllocator::kInvalid);
}

// The whole point of the class: without reclamation this is the leak that made
// every RemoveTexture consume a slot forever.
TEST_CASE(DescriptorAllocator_ReleasedSlotIsReusedAfterFence)
{
    render::DescriptorAllocator a;
    a.Init(4, 1);

    const uint32_t first = a.Allocate();   // 1
    a.Allocate();                          // 2
    a.Allocate();                          // 3
    CHECK_EQ(a.Allocate(), render::DescriptorAllocator::kInvalid);   // full

    a.Release(first, 10);

    // Still full: the GPU has not passed fence 10, so the slot is NOT reusable.
    CHECK_EQ(a.Allocate(), render::DescriptorAllocator::kInvalid);
    CHECK_EQ(a.PendingCount(), (size_t)1);
    CHECK_EQ(a.FreeCount(), (size_t)0);

    CHECK_EQ(a.Reclaim(9), (size_t)0);     // 9 < 10, still not safe
    CHECK_EQ(a.Allocate(), render::DescriptorAllocator::kInvalid);

    CHECK_EQ(a.Reclaim(10), (size_t)1);    // boundary: fenceValue == completed
    CHECK_EQ(a.FreeCount(), (size_t)1);
    CHECK_EQ(a.Allocate(), first);         // the same slot comes back
}

// This is the use-after-free case stated directly: a slot released while a
// command list referencing it is still in flight must not be handed out.
TEST_CASE(DescriptorAllocator_NeverReusesSlotWhileFrameInFlight)
{
    render::DescriptorAllocator a;
    a.Init(3, 0);

    const uint32_t slot = a.Allocate();   // 0

    // Frame 7 recorded a draw referencing `slot`, then the texture was removed.
    a.Release(slot, 7);

    // Frames 5 and 6 have retired; frame 7 has not.
    for (uint64_t completed = 0; completed < 7; ++completed)
    {
        CHECK_EQ(a.Reclaim(completed), (size_t)0);
        CHECK_EQ(a.FreeCount(), (size_t)0);
    }

    CHECK_EQ(a.Reclaim(7), (size_t)1);
    CHECK_EQ(a.Allocate(), slot);
}

TEST_CASE(DescriptorAllocator_OutOfOrderReleaseDelaysRatherThanFreesEarly)
{
    render::DescriptorAllocator a;
    a.Init(8, 0);

    const uint32_t late  = a.Allocate();   // 0, released against a far fence
    const uint32_t early = a.Allocate();   // 1, released against a near fence

    a.Release(late, 100);
    a.Release(early, 1);

    // Reclaiming at 50 must free `early` only. `late` must survive even though
    // it sits ahead of `early` in the queue.
    CHECK_EQ(a.Reclaim(50), (size_t)1);
    CHECK_EQ(a.PendingCount(), (size_t)1);
    CHECK_EQ(a.Allocate(), early);

    CHECK_EQ(a.Reclaim(100), (size_t)1);
    CHECK_EQ(a.Allocate(), late);
}

TEST_CASE(DescriptorAllocator_RejectsReservedAndOutOfRangeReleases)
{
    render::DescriptorAllocator a;
    a.Init(4, 1);
    a.Allocate();   // 1

    // Releasing the reserved null-SRV slot would let a material overwrite the
    // fallback every other subsystem relies on.
    a.Release(0, 1);
    CHECK_EQ(a.PendingCount(), (size_t)0);

    a.Release(99, 1);                                       // past capacity
    a.Release(render::DescriptorAllocator::kInvalid, 1);    // a failed Allocate
    CHECK_EQ(a.PendingCount(), (size_t)0);

    CHECK_EQ(a.Reclaim(1000), (size_t)0);
    CHECK_EQ(a.FreeCount(), (size_t)0);
}

TEST_CASE(DescriptorAllocator_RejectsDuplicateAndNeverAllocatedReleases)
{
    render::DescriptorAllocator a;
    a.Init(8, 1);

    const uint32_t live = a.Allocate();
    CHECK(a.Release(live, 5));
    CHECK_FALSE(a.Release(live, 5));
    CHECK_FALSE(a.Release(6, 5));
    CHECK_EQ(a.PendingCount(), (size_t)1);
    CHECK_EQ(a.InUse(), (uint32_t)0);

    CHECK_EQ(a.Reclaim(5), (size_t)1);
    CHECK_FALSE(a.Release(live, 6));

    const uint32_t recycled = a.Allocate();
    const uint32_t fresh = a.Allocate();
    CHECK_EQ(recycled, live);
    CHECK(fresh != live);
    CHECK_EQ(a.InUse(), (uint32_t)2);
}

TEST_CASE(DescriptorAllocator_DuplicateReleaseCannotAliasLiveSlots)
{
    render::DescriptorAllocator a;
    a.Init(4, 0);

    const uint32_t released = a.Allocate();
    CHECK(a.Release(released, 1));
    CHECK_FALSE(a.Release(released, 1));
    CHECK_EQ(a.Reclaim(1), (size_t)1);

    const uint32_t first = a.Allocate();
    const uint32_t second = a.Allocate();
    CHECK_EQ(first, released);
    CHECK(second != first);
}

TEST_CASE(DescriptorAllocator_ReclaimAllDrainsRegardlessOfFence)
{
    render::DescriptorAllocator a;
    a.Init(8, 0);
    const uint32_t x = a.Allocate();
    const uint32_t y = a.Allocate();
    a.Release(x, 5000);
    a.Release(y, 6000);

    // Device-loss / shutdown: the fence will never reach those values.
    CHECK_EQ(a.PendingCount(), (size_t)2);
    a.ReclaimAll();
    CHECK_EQ(a.PendingCount(), (size_t)0);
    CHECK_EQ(a.FreeCount(), (size_t)2);
}

TEST_CASE(DescriptorAllocator_InUseAccounting)
{
    render::DescriptorAllocator a;
    a.Init(16, 1);
    CHECK_EQ(a.InUse(), (uint32_t)0);

    const uint32_t s1 = a.Allocate();
    a.Allocate();
    a.Allocate();
    CHECK_EQ(a.InUse(), (uint32_t)3);

    a.Release(s1, 42);
    CHECK_EQ(a.InUse(), (uint32_t)2);      // pending is not in use
    CHECK_EQ(a.PendingCount(), (size_t)1);

    a.Reclaim(42);
    CHECK_EQ(a.InUse(), (uint32_t)2);      // still 2; the slot is now free
    CHECK_EQ(a.FreeCount(), (size_t)1);

    a.Allocate();
    CHECK_EQ(a.InUse(), (uint32_t)3);
    CHECK_EQ(a.HighWater(), (uint32_t)4);  // recycled, so no new high water
}

// Churn must not grow the heap: a steady add/remove cycle should keep reusing
// the same slots rather than marching the high-water mark toward capacity.
TEST_CASE(DescriptorAllocator_ChurnDoesNotGrowHighWater)
{
    render::DescriptorAllocator a;
    a.Init(128, 1);

    uint64_t fence = 0;
    uint32_t live = a.Allocate();
    const uint32_t highWaterAfterFirst = a.HighWater();

    for (int i = 0; i < 500; ++i)
    {
        ++fence;
        a.Release(live, fence);
        a.Reclaim(fence);
        live = a.Allocate();
        CHECK(live != render::DescriptorAllocator::kInvalid);
    }

    // 500 add/remove cycles through a 128-slot heap: without reclamation this
    // would have exhausted it four times over.
    CHECK_EQ(a.HighWater(), highWaterAfterFirst);
    CHECK_EQ(a.InUse(), (uint32_t)1);
}
