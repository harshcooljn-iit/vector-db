// SPDX-License-Identifier: MIT
#include <algorithm>

#include <vectordb/concurrency/thread_pool.hpp>

namespace vectordb {

std::size_t hardware_threads() noexcept {
    const unsigned int reported = std::thread::hardware_concurrency();
    // The standard permits 0 for "cannot determine", and a pool of zero threads
    // would silently never run anything.
    return reported == 0 ? 1 : static_cast<std::size_t>(reported);
}

ThreadPool::ThreadPool(std::size_t threads) {
    const std::size_t count = threads == 0 ? hardware_threads() : threads;
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    // notify_all, not notify_one: every worker is waiting and every one must
    // wake to observe the flag and exit. Waking one would leave the rest
    // blocked forever and join() would never return.
    work_available_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            // The predicate form of wait() handles spurious wakeups, which are
            // real and permitted by the standard: a bare wait() can return with
            // the queue still empty, and popping from it would be undefined.
            work_available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });

            // Drain before exiting, so a pool destroyed immediately after
            // submit() still runs the work rather than dropping it.
            if (stopping_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
            ++active_;
        }

        // Run outside the lock. Holding it here would serialise every task and
        // turn the pool into an expensive way to run things one at a time.
        task();

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            --active_;
            if (tasks_.empty() && active_ == 0) {
                all_idle_.notify_all();
            }
        }
    }
}

void ThreadPool::parallel_for(std::size_t count, const std::function<void(std::size_t)>& body) {
    if (count == 0) {
        return;
    }

    // Run inline when there is nothing to gain. Below this threshold the
    // synchronisation costs more than the work, and a caller should not have to
    // decide that for themselves.
    if (workers_.empty() || count == 1) {
        for (std::size_t i = 0; i < count; ++i) {
            body(i);
        }
        return;
    }

    // Contiguous chunks, one per worker. Per-index tasks would pay a queue
    // push, a mutex acquisition and a notify for each one — comparable to the
    // cost of a short body.
    const std::size_t chunks = std::min(count, workers_.size());
    const std::size_t chunk_size = (count + chunks - 1) / chunks;

    std::vector<std::future<void>> futures;
    futures.reserve(chunks);

    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
        const std::size_t begin = chunk * chunk_size;
        const std::size_t end = std::min(begin + chunk_size, count);
        if (begin >= end) {
            break;
        }
        futures.push_back(submit([&body, begin, end] {
            for (std::size_t i = begin; i < end; ++i) {
                body(i);
            }
        }));
    }

    // get() rather than wait(), so an exception from any chunk propagates to
    // the caller instead of being silently dropped. The first one wins; the
    // rest are discarded, which is the usual parallel-algorithm behaviour.
    for (std::future<void>& future : futures) {
        future.get();
    }
}

void ThreadPool::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    all_idle_.wait(lock, [this] { return tasks_.empty() && active_ == 0; });
}

std::size_t ThreadPool::pending() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

}  // namespace vectordb
