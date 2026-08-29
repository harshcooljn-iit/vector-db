// SPDX-License-Identifier: MIT
//
// A fixed set of worker threads consuming a task queue.
//
// Learning note: learnings/60-concurrency/04-thread-pools.md
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace vectordb {

/// Threads this machine can run at once, or 1 if the runtime will not say.
[[nodiscard]] std::size_t hardware_threads() noexcept;

/// A work-queue thread pool.
///
/// ## Why not one thread per query?
///
/// Creating a thread costs on the order of 10-100 microseconds: the kernel
/// allocates a stack (often 512 KB to 8 MB of address space), sets up a task
/// structure, and schedules it. A search takes tens of microseconds. You would
/// spend more time hiring the worker than doing the work.
///
/// Worse, threads are not free to *have*: each one competes for cores, and a
/// thousand concurrent queries would create a thousand threads on eight cores,
/// where the scheduler thrashes and everything slows down together.
///
/// A pool creates N threads once and feeds them from a queue. Threads are a
/// resource to be pooled, exactly like database connections.
///
/// ## Why not spin?
///
/// A worker with nothing to do could poll the queue in a loop. That burns a
/// full core doing nothing, and on a laptop it burns battery and heats the
/// machine into thermal throttling — slowing down the threads that *are*
/// working. Workers block on a condition variable instead: the OS removes them
/// from the run queue entirely and wakes them when work arrives.
///
/// ## Thread safety
///
/// All public methods are safe to call from any thread, including from inside a
/// task — with the caveat noted on `parallel_for`.
class ThreadPool {
public:
    /// @param threads  0 means `hardware_threads()`
    explicit ThreadPool(std::size_t threads = 0);

    /// Waits for queued work to finish, then joins every worker.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Queues `task` and returns a future for its result.
    ///
    /// An exception thrown by the task is captured in the future and rethrown
    /// on `get()`. A worker thread that let an exception escape would call
    /// `std::terminate` and take the process with it.
    template<typename Fn>
    auto submit(Fn&& task) -> std::future<std::invoke_result_t<Fn>>;

    /// Runs `body(i)` for every `i` in `[0, count)` and waits for all of them.
    ///
    /// Work is handed out in contiguous chunks rather than one index per task:
    /// per-task overhead is a queue push, a mutex acquisition and a condition
    /// variable notify, which is comparable to the cost of one short body.
    ///
    /// **Do not call this from inside a task on the same pool.** Every worker
    /// would block waiting for work that only a worker can run. This pool has
    /// no work-stealing and does not detect the deadlock — it is documented
    /// rather than guarded, because the check would cost something on every
    /// call to prevent a mistake the API shape already discourages.
    void parallel_for(std::size_t count, const std::function<void(std::size_t)>& body);

    /// Blocks until every queued and running task has finished.
    void wait_idle();

    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }

    /// Tasks queued but not yet started. Diagnostic only — it is stale the
    /// instant it is returned.
    [[nodiscard]] std::size_t pending() const;

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable all_idle_;

    std::size_t active_ = 0;
    bool stopping_ = false;
};

template<typename Fn>
auto ThreadPool::submit(Fn&& task) -> std::future<std::invoke_result_t<Fn>> {
    using Result = std::invoke_result_t<Fn>;

    // shared_ptr because std::function requires a copyable target and
    // packaged_task is move-only.
    auto packaged = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(task));
    std::future<Result> future = packaged->get_future();

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            throw std::runtime_error("thread pool is shutting down");
        }
        tasks_.emplace([packaged] { (*packaged)(); });
    }
    work_available_.notify_one();
    return future;
}

}  // namespace vectordb
