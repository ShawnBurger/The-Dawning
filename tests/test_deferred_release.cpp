// =============================================================================
// tests/test_deferred_release.cpp — fence-guarded release queue
// =============================================================================
// Covers render/deferred_release.h, which is deliberately D3D12-free so this
// bookkeeping can be tested without a GPU. The rule it enforces is asymmetric:
// releasing LATE is merely a delayed free, releasing EARLY is a use-after-free.
// These tests are written from that asymmetry.
// =============================================================================

#include "test_framework.h"
#include "render/deferred_release.h"

#include <memory>

// Payload instrumentation note: these tests assert on shared_ptr::use_count,
// NOT on destructor calls. That is deliberate and models ComPtr correctly.
// std::vector::erase shifts survivors down by MOVE-ASSIGNMENT and only destroys
// the tail, so a released element is overwritten without its destructor running
// — counting destructor calls under-reports by exactly one. A refcount drop is
// also literally what releasing a ComPtr does.

TEST_CASE(DeferredRelease_EmptyQueueIsInert)
{
    render::DeferredReleaseQueue<int> q;
    CHECK(q.Empty());
    CHECK_EQ(q.Size(), (size_t)0);
    CHECK_EQ(q.Process(1000), (size_t)0);
    CHECK_EQ(q.Clear(), (size_t)0);
}

TEST_CASE(DeferredRelease_HoldsUntilFenceReached)
{
    render::DeferredReleaseQueue<int> q;
    q.Push(10, 1);
    q.Push(20, 2);
    CHECK_EQ(q.Size(), (size_t)2);

    // GPU has not reached either fence value.
    CHECK_EQ(q.Process(9), (size_t)0);
    CHECK_EQ(q.Size(), (size_t)2);

    // Boundary: fenceValue == completed must release. The fence value recorded
    // is the one that must COMPLETE, so equality means it is done.
    CHECK_EQ(q.Process(10), (size_t)1);
    CHECK_EQ(q.Size(), (size_t)1);

    CHECK_EQ(q.Process(20), (size_t)1);
    CHECK(q.Empty());
}

TEST_CASE(DeferredRelease_ReleasesPrefixOnly)
{
    render::DeferredReleaseQueue<int> q;
    for (uint64_t f = 1; f <= 5; ++f) q.Push(f, (int)f);

    // Completing 3 releases exactly the first three, leaving 4 and 5.
    CHECK_EQ(q.Process(3), (size_t)3);
    CHECK_EQ(q.Size(), (size_t)2);

    CHECK_EQ(q.Process(100), (size_t)2);
    CHECK(q.Empty());
}

// The payload must actually be destroyed at release, not merely unlinked. With
// ComPtr this is the difference between freeing the GPU resource and leaking it.
TEST_CASE(DeferredRelease_DropsPayloadReferenceOnRelease)
{
    auto first  = std::make_shared<int>(1);
    auto second = std::make_shared<int>(2);
    {
        render::DeferredReleaseQueue<std::shared_ptr<int>> q;
        q.Push(5, first);
        q.Push(15, second);

        // Queue holds a reference to each, alongside the local.
        CHECK_EQ(first.use_count(), (long)2);
        CHECK_EQ(second.use_count(), (long)2);

        q.Process(5);
        CHECK_EQ(first.use_count(), (long)1);    // released
        CHECK_EQ(second.use_count(), (long)2);   // still held

        q.Process(15);
        CHECK_EQ(second.use_count(), (long)1);
    }
    // Queue destruction must not have kept anything alive.
    CHECK_EQ(first.use_count(), (long)1);
    CHECK_EQ(second.use_count(), (long)1);
}

TEST_CASE(DeferredRelease_ClearReleasesEverythingUnconditionally)
{
    auto a = std::make_shared<int>(1);
    auto b = std::make_shared<int>(2);
    render::DeferredReleaseQueue<std::shared_ptr<int>> q;
    q.Push(1000, a);
    q.Push(2000, b);

    // Nothing has completed, but Clear is the device-lost / shutdown path: a lost
    // device never advances its fence again, so waiting would leak to exit.
    CHECK_EQ(q.Clear(), (size_t)2);
    CHECK_EQ(a.use_count(), (long)1);
    CHECK_EQ(b.use_count(), (long)1);
    CHECK(q.Empty());
}

// Callers push in non-decreasing fence order, but the implementation must not
// release something early if that ever stops holding. Stopping at the first
// entry it cannot release trades a delayed free for safety.
TEST_CASE(DeferredRelease_OutOfOrderPushDelaysRatherThanReleasesEarly)
{
    auto blocker = std::make_shared<int>(1);
    auto behind  = std::make_shared<int>(2);
    render::DeferredReleaseQueue<std::shared_ptr<int>> q;
    q.Push(100, blocker);   // not yet reached
    q.Push(1,   behind);    // reached, but sits behind the blocker

    // Must release nothing: the head is not releasable, and the entry behind it
    // must not be reordered past it. Releasing late is a delayed free; releasing
    // early is a use-after-free.
    CHECK_EQ(q.Process(50), (size_t)0);
    CHECK_EQ(blocker.use_count(), (long)2);
    CHECK_EQ(behind.use_count(), (long)2);

    CHECK_EQ(q.Process(100), (size_t)2);
    CHECK_EQ(blocker.use_count(), (long)1);
    CHECK_EQ(behind.use_count(), (long)1);
}

// A frame that retires nothing must not disturb entries already queued.
TEST_CASE(DeferredRelease_RepeatedProcessIsIdempotent)
{
    render::DeferredReleaseQueue<int> q;
    q.Push(7, 42);

    CHECK_EQ(q.Process(3), (size_t)0);
    CHECK_EQ(q.Process(3), (size_t)0);
    CHECK_EQ(q.Size(), (size_t)1);

    CHECK_EQ(q.Process(7), (size_t)1);
    CHECK_EQ(q.Process(7), (size_t)0);   // already gone; must not underflow
    CHECK(q.Empty());
}
