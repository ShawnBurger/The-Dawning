#include "job_system.h"

#include <algorithm>

namespace core
{

JobSystem::JobSystem(unsigned threadCount)
{
    unsigned n = threadCount;
    if (n == 0)
    {
        const unsigned hc = std::thread::hardware_concurrency();
        n = (hc > 1) ? (hc - 1) : 1; // leave one core for the caller; always >= 1
    }
    m_threads.reserve(n);
    try
    {
        for (unsigned i = 0; i < n; ++i)
            m_threads.emplace_back([this] { WorkerLoop(); });
    }
    catch (...)
    {
        // A partially constructed vector of joinable std::threads would call
        // std::terminate while unwinding. Stop and join the workers that did
        // start, then preserve the original construction failure.
        {
            std::lock_guard<std::mutex> lk(m_queueMutex);
            m_stop = true;
        }
        m_queueCv.notify_all();
        for (std::thread& thread : m_threads)
            if (thread.joinable()) thread.join();
        throw;
    }
}

JobSystem::~JobSystem()
{
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        m_stop = true;
    }
    m_queueCv.notify_all();
    for (std::thread& t : m_threads)
        if (t.joinable()) t.join();
}

void JobSystem::WorkerLoop()
{
    for (;;)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_queueMutex);
            m_queueCv.wait(lk, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty())
                return;
            task = std::move(m_queue.front());
            m_queue.pop();
        }
        task();
        // Decrement AFTER the task completes, so Wait() only releases once every
        // side effect is done. fetch_sub returns the pre-decrement value.
        if (m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            std::lock_guard<std::mutex> lk(m_doneMutex);
            m_doneCv.notify_all();
        }
    }
}

void JobSystem::Dispatch(uint32_t jobCount, uint32_t groupSize,
                         const std::function<void(uint32_t)>& job)
{
    if (jobCount == 0 || !job)
        return;
    if (groupSize == 0)
        groupSize = 1;
    // Overflow-safe ceiling division. (jobCount + groupSize - 1) wraps for a
    // perfectly valid large groupSize and used to turn a non-empty dispatch into
    // zero queued work.
    const uint32_t groups = 1u + (jobCount - 1u) / groupSize;

    // Publish the outstanding count BEFORE enqueuing, so a worker that finishes a
    // task can never drive pending to 0 before every task of this dispatch is counted.
    m_pending.fetch_add(groups, std::memory_order_acq_rel);
    uint32_t enqueued = 0;
    try
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        for (uint32_t g = 0; g < groups; ++g)
        {
            const uint32_t begin = g * groupSize;
            const uint32_t end = begin + (std::min)(groupSize, jobCount - begin);
            m_queue.emplace([job, begin, end] {
                for (uint32_t i = begin; i < end; ++i)
                    job(i);
            });
            ++enqueued;
        }
    }
    catch (...)
    {
        // Queue allocation/callable-copy failure may happen after a prefix was
        // committed. Remove the never-enqueued suffix from the barrier count so
        // Wait cannot hang; the prefix remains valid work and is woken below.
        const uint64_t omitted = static_cast<uint64_t>(groups - enqueued);
        const uint64_t previous = m_pending.fetch_sub(omitted, std::memory_order_acq_rel);
        if (previous == omitted)
        {
            std::lock_guard<std::mutex> lk(m_doneMutex);
            m_doneCv.notify_all();
        }
        if (enqueued != 0) m_queueCv.notify_all();
        throw;
    }
    m_queueCv.notify_all();
}

void JobSystem::ParallelFor(uint32_t count, const std::function<void(uint32_t)>& body,
                            uint32_t groupSize)
{
    Dispatch(count, groupSize, body);
    Wait();
}

void JobSystem::Wait()
{
    std::unique_lock<std::mutex> lk(m_doneMutex);
    m_doneCv.wait(lk, [this] { return m_pending.load(std::memory_order_acquire) == 0; });
}

} // namespace core
