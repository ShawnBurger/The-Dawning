#pragma once
// =============================================================================
// core/job_system.h - a minimal, correct, deterministic-result thread pool
// =============================================================================
// Backlog "stabilize" item: the engine has no worker threads yet, and RULE 8
// (CLAUDE.md) keeps the logger single-threaded "until a job system exists". This
// is that job system - a small, well-tested thread pool other subsystems can
// stand on (the N-body force loop, collision broadphase, and asset cooking are
// all embarrassingly parallel and determinism-friendly).
//
// THE ONE PROPERTY THAT MATTERS FOR THIS ENGINE: ParallelFor's RESULT is
// deterministic even though thread SCHEDULING is not. Each index in [0,count) is
// executed EXACTLY ONCE; when the per-index work is independent (writes its own
// slot), the output is identical regardless of thread count or scheduling. That
// keeps the fixed-step sim's same-binary determinism (RULE 6) intact when a loop
// is later parallelised: parallelise the map, keep any REDUCTION in a fixed
// bodyId/index order on one thread. This class deliberately provides only the
// map (ParallelFor); it never reduces, so it can never reorder a float sum.
//
// Scope: a global-queue pool (mutex + condition_variable), not a work-stealing
// scheduler - correctness and testability first. Do NOT call ParallelFor/Wait
// from inside a running job (no nested parallelism; a worker blocking on its own
// pool would deadlock). CPU-only, GPU-free; links into TheDawningV3 and the tests.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace core
{

class JobSystem
{
public:
    // threadCount == 0 => choose max(1, hardware_concurrency() - 1) (leave one core
    // for the main/render thread). Always at least one worker.
    explicit JobSystem(unsigned threadCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    unsigned ThreadCount() const { return static_cast<unsigned>(m_threads.size()); }

    // Enqueue `jobCount` work items, split into groups of `groupSize` consecutive
    // indices per queued task (groupSize 0 is treated as 1). An empty callback is
    // a no-op. ASYNC: returns once the tasks are queued; call Wait() to block for
    // completion. `job(i)` is called once for every i in [0, jobCount). A job MUST
    // NOT throw (a throwing job
    // std::terminate()s a worker) and MUST NOT touch shared state without its own
    // synchronisation - independent per-index writes are the intended use.
    void Dispatch(uint32_t jobCount, uint32_t groupSize,
                  const std::function<void(uint32_t index)>& job);

    // Dispatch + Wait. body(i) for every i in [0,count). Deterministic result for
    // independent bodies. groupSize batches indices to amortise per-task overhead.
    void ParallelFor(uint32_t count, const std::function<void(uint32_t index)>& body,
                     uint32_t groupSize = 1);

    // Block the calling thread until every dispatched task has finished.
    void Wait();

    // True if any dispatched task is still outstanding.
    bool Busy() const { return m_pending.load(std::memory_order_acquire) != 0; }

private:
    void WorkerLoop();

    std::vector<std::thread>          m_threads;
    std::queue<std::function<void()>> m_queue;
    std::mutex                        m_queueMutex;
    std::condition_variable           m_queueCv;      // signals: work available / stop
    std::mutex                        m_doneMutex;
    std::condition_variable           m_doneCv;       // signals: pending reached 0
    std::atomic<uint64_t>             m_pending{ 0 }; // outstanding queued tasks
    bool                              m_stop = false;
};

} // namespace core
