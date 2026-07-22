// =============================================================================
// tests/test_job_system.cpp - the core thread-pool job system
// =============================================================================
// Drives the SHIPPED src/core/job_system.{h,cpp}. Threads are non-deterministic
// in SCHEDULING; the property asserted here is that ParallelFor's RESULT is
// deterministic anyway - each index runs exactly once, independent work produces
// the same output regardless of thread count, and Wait() is a real barrier. Those
// are the guarantees the fixed-step sim needs before any loop is parallelised.
// =============================================================================

#include "test_framework.h"
#include "core/job_system.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <set>
#include <thread>
#include <vector>

namespace
{
// A deterministic, non-trivial per-index function (no cross-index dependence).
double F(uint32_t i)
{
    const double x = static_cast<double>(i);
    return std::sin(x * 0.001) * 3.0 + x * 1.5 - std::cos(x * 0.01);
}
} // namespace

// =============================================================================
// T1 - RESULT == SERIAL, and BIT-EXACT REPEATABLE across many runs (determinism
//      of the result despite non-deterministic scheduling).
// =============================================================================
TEST_CASE(JobSystem_ParallelFor_MatchesSerial_AndIsRepeatable)
{
    core::JobSystem js;
    const uint32_t N = 20000;

    std::vector<double> serial(N);
    for (uint32_t i = 0; i < N; ++i) serial[i] = F(i);

    std::vector<double> par(N, 0.0);
    js.ParallelFor(N, [&](uint32_t i) { par[i] = F(i); }, /*groupSize*/ 64);

    bool match = true;
    for (uint32_t i = 0; i < N; ++i)
        if (std::bit_cast<uint64_t>(par[i]) != std::bit_cast<uint64_t>(serial[i])) { match = false; break; }
    CHECK(match); // independent map == serial, bit-exact

    // Repeat many times; every run must be bit-identical (scheduling cannot change it).
    bool allSame = true;
    for (int r = 0; r < 100; ++r)
    {
        std::vector<double> again(N, -1.0);
        js.ParallelFor(N, [&](uint32_t i) { again[i] = F(i); }, 128);
        for (uint32_t i = 0; i < N; ++i)
            if (std::bit_cast<uint64_t>(again[i]) != std::bit_cast<uint64_t>(serial[i])) { allSame = false; break; }
        if (!allSame) break;
    }
    CHECK(allSame);
}

// =============================================================================
// T2 - EVERY INDEX RUNS EXACTLY ONCE, across group sizes that do not divide the
//      count evenly (catches an off-by-one in the range split).
// =============================================================================
TEST_CASE(JobSystem_EveryIndexRunsExactlyOnce)
{
    core::JobSystem js;
    for (uint32_t groupSize : { 1u, 7u, 64u, 1000u })
    {
        const uint32_t N = 3001; // prime-ish; not a multiple of any group above
        std::vector<int> visits(N, 0);
        std::atomic<uint32_t> total{ 0 };
        js.ParallelFor(N, [&](uint32_t i) { visits[i] += 1; total.fetch_add(1); }, groupSize);
        CHECK_EQ(total.load(), N);                 // exactly N executions
        bool eachOnce = true;
        for (uint32_t i = 0; i < N; ++i) if (visits[i] != 1) { eachOnce = false; break; }
        CHECK(eachOnce);                            // no index skipped or double-run
    }
}

// =============================================================================
// T3 - RACE-FREE ACCUMULATION + Wait() IS A REAL BARRIER. If Wait returned early
//      (or dispatch/complete counting were wrong) the atomic total would be short.
// =============================================================================
TEST_CASE(JobSystem_RaceFreeAccumulation_AndWaitBarrier)
{
    core::JobSystem js;
    const uint32_t N = 100000;
    std::atomic<uint64_t> sum{ 0 };
    js.ParallelFor(N, [&](uint32_t i) { sum.fetch_add(i); }, 32);
    CHECK_FALSE(js.Busy());                         // Wait() drained everything
    uint64_t expected = 0; for (uint64_t i = 0; i < N; ++i) expected += i;
    CHECK_EQ(sum.load(), expected);                 // every job's effect is visible after Wait
}

// =============================================================================
// T4 - CONCURRENCY ACTUALLY HAPPENS: on a multi-core host the pool spreads work
//      across more than one worker thread. A per-job spin makes the spread
//      reliable; skipped when the host/pool is single-threaded.
// =============================================================================
TEST_CASE(JobSystem_UsesMultipleThreads_WhenAvailable)
{
    core::JobSystem js;
    const uint32_t N = 8192;
    std::vector<std::thread::id> ranOn(N);
    js.ParallelFor(N, [&](uint32_t i) {
        // Tiny busy-work so tasks overlap and spread across workers.
        volatile double acc = 0.0;
        for (int k = 0; k < 200; ++k) acc += std::sin(double(i + k));
        (void)acc;
        ranOn[i] = std::this_thread::get_id();
    }, /*groupSize*/ 1);

    std::set<std::thread::id> distinct(ranOn.begin(), ranOn.end());
    if (js.ThreadCount() >= 2 && std::thread::hardware_concurrency() >= 2)
        CHECK(distinct.size() >= 2);   // genuine concurrency, not everything on one thread
    else
        CHECK(distinct.size() >= 1);   // single-worker host: still valid
}

// =============================================================================
// T5 - GRACEFUL DEGENERATE CASES: empty dispatch, single-worker pool, and
//      back-to-back dispatches all behave.
// =============================================================================
TEST_CASE(JobSystem_DegenerateCases)
{
    // Empty ParallelFor is a no-op and does not hang.
    {
        core::JobSystem js;
        int touched = 0;
        js.ParallelFor(0, [&](uint32_t) { touched = 1; });
        CHECK_EQ(touched, 0);
        CHECK_FALSE(js.Busy());
    }
    // A forced single-worker pool still runs everything exactly once.
    {
        core::JobSystem js(1);
        CHECK_EQ(js.ThreadCount(), 1u);
        const uint32_t N = 5000;
        std::atomic<uint32_t> c{ 0 };
        js.ParallelFor(N, [&](uint32_t) { c.fetch_add(1); });
        CHECK_EQ(c.load(), N);
    }
    // Back-to-back dispatches accumulate correctly under one Wait each.
    {
        core::JobSystem js;
        std::atomic<uint64_t> s{ 0 };
        for (int round = 0; round < 5; ++round)
            js.ParallelFor(1000, [&](uint32_t) { s.fetch_add(1); }, 16);
        CHECK_EQ(s.load(), 5000ull);
    }
}

TEST_CASE(JobSystem_EmptyCallbackIsANoop)
{
    core::JobSystem js(1);
    js.Dispatch(3, 1, std::function<void(uint32_t)>{});
    js.Wait();
    CHECK_FALSE(js.Busy());
}

TEST_CASE(JobSystem_LargeGroupSizeDoesNotOverflowCeilingDivision)
{
    core::JobSystem js(1);
    std::atomic<uint32_t> visits{ 0 };
    js.ParallelFor(3, [&](uint32_t) { visits.fetch_add(1); }, UINT32_MAX);
    CHECK_EQ(visits.load(), 3u);
    CHECK_FALSE(js.Busy());
}

// =============================================================================
// T6 - THE INTENDED DETERMINISM PATTERN: parallelise the MAP, keep the REDUCTION
//      in fixed index order on one thread => bit-exact with a fully serial sum.
//      This is how a sim loop keeps same-binary determinism when parallelised.
// =============================================================================
TEST_CASE(JobSystem_MapThenFixedOrderReduce_IsBitExact)
{
    core::JobSystem js;
    const uint32_t N = 4096;

    // Parallel map into per-index slots (no shared reduction on the workers).
    std::vector<double> partial(N, 0.0);
    js.ParallelFor(N, [&](uint32_t i) { partial[i] = F(i); }, 32);
    // Fixed-order (index-ascending) reduction on ONE thread.
    double parReduce = 0.0;
    for (uint32_t i = 0; i < N; ++i) parReduce += partial[i];

    // Fully serial reference in the SAME order.
    double serReduce = 0.0;
    for (uint32_t i = 0; i < N; ++i) serReduce += F(i);

    CHECK_EQ(std::bit_cast<uint64_t>(parReduce), std::bit_cast<uint64_t>(serReduce)); // bit-exact
}
