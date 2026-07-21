// =============================================================================
// tests/test_log.cpp - logger thread-safety (concurrency smoke)
// =============================================================================
// Drives the SHIPPED src/core/log.cpp. Now that core::JobSystem exists, the logger
// must be safe to call from worker threads (RULE 8's "main-thread only until a job
// system exists" no longer applies).
//
// HONEST SCOPE - what this test does and does NOT prove: it fires many concurrent
// Error()/Info()/Warn() calls through the job system and asserts the process does
// not crash and the atomic error count is exact. It does NOT witness the atomic vs
// a non-atomic counter: the logger's output I/O (OutputDebugStringA syscall + the
// CRT's FILE* lock on fputs) already serialises concurrent calls in practice, so the
// counter race is unobservable from a unit test even fully unsynchronised, and MSVC
// has no ThreadSanitizer to flag the data race directly. The atomic + output mutex
// remove the *theoretical* UB by construction; this test is the crash-free
// concurrency smoke that stands alongside that, not a watched-failing witness.
// =============================================================================

#include "test_framework.h"
#include "core/log.h"
#include "core/job_system.h"

#include <cstdint>

TEST_CASE(Log_ConcurrentLogging_CrashFree_AndCountExact)
{
    core::JobSystem js;

    // Many concurrent Error() calls: no crash, and the atomic count is exact.
    const uint32_t before = core::Log::ErrorCount();
    const uint32_t N = 1500;
    js.ParallelFor(N, [](uint32_t) { core::Log::Error("concurrent-error"); }, 1);
    CHECK_EQ(core::Log::ErrorCount() - before, N);

    // Concurrent Info/Warn: no crash, and non-error levels leave the count untouched.
    const uint32_t afterErrors = core::Log::ErrorCount();
    js.ParallelFor(300, [](uint32_t i) {
        if (i & 1u) core::Log::Info("concurrent-info");
        else        core::Log::Warn("concurrent-warn");
    }, 1);
    CHECK_EQ(core::Log::ErrorCount(), afterErrors);
}
