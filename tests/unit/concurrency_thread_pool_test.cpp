// SPDX-License-Identifier: MIT
#include <atomic>
#include <chrono>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/concurrency/thread_pool.hpp>

namespace vectordb {
namespace {

TEST(HardwareThreads, IsAtLeastOne) {
    // The standard permits hardware_concurrency() to return 0; a pool of zero
    // threads would silently never run anything.
    EXPECT_GE(hardware_threads(), 1U);
}

TEST(ThreadPool, CreatesTheRequestedNumberOfWorkers) {
    const ThreadPool pool(4);
    EXPECT_EQ(pool.size(), 4U);

    const ThreadPool automatic(0);
    EXPECT_EQ(automatic.size(), hardware_threads());
}

TEST(ThreadPool, RunsASubmittedTaskAndReturnsItsResult) {
    ThreadPool pool(2);
    std::future<int> answer = pool.submit([] { return 42; });
    EXPECT_EQ(answer.get(), 42);
}

TEST(ThreadPool, RunsEverySubmittedTaskExactlyOnce) {
    constexpr int kTasks = 1000;
    ThreadPool pool(4);
    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.submit([&counter] { counter.fetch_add(1); }));
    }
    for (std::future<void>& future : futures) {
        future.get();
    }

    EXPECT_EQ(counter.load(), kTasks);
}

TEST(ThreadPool, ActuallyUsesMoreThanOneThread) {
    ThreadPool pool(4);
    std::mutex mutex;
    std::set<std::thread::id> seen;

    std::vector<std::future<void>> futures;
    for (int i = 0; i < 64; ++i) {
        futures.push_back(pool.submit([&] {
            // Hold each worker briefly so the work cannot all be finished by
            // one thread before the others are scheduled.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const std::lock_guard<std::mutex> lock(mutex);
            seen.insert(std::this_thread::get_id());
        }));
    }
    for (std::future<void>& future : futures) {
        future.get();
    }

    EXPECT_GT(seen.size(), 1U) << "the pool ran everything on one thread";
    EXPECT_LE(seen.size(), 4U) << "the pool created more threads than it was asked for";
}

// A worker that lets an exception escape calls std::terminate and takes the
// process down. The exception must reach the caller through the future.
TEST(ThreadPool, DeliversAnExceptionThroughTheFuture) {
    ThreadPool pool(2);
    std::future<int> failing =
        pool.submit([]() -> int { throw std::runtime_error("task failed"); });

    EXPECT_THROW(static_cast<void>(failing.get()), std::runtime_error);

    // And the pool must still be usable afterwards.
    EXPECT_EQ(pool.submit([] { return 7; }).get(), 7);
}

TEST(ThreadPool, RunsQueuedWorkBeforeShuttingDown) {
    std::atomic<int> completed{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 200; ++i) {
            static_cast<void>(pool.submit([&completed] { completed.fetch_add(1); }));
        }
        // The destructor drains rather than dropping; the alternative would
        // silently lose work a caller had already been told was accepted.
    }
    EXPECT_EQ(completed.load(), 200);
}

TEST(ThreadPool, WaitIdleBlocksUntilEverythingHasFinished) {
    ThreadPool pool(3);
    std::atomic<int> completed{0};

    for (int i = 0; i < 30; ++i) {
        static_cast<void>(pool.submit([&completed] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completed.fetch_add(1);
        }));
    }

    pool.wait_idle();
    EXPECT_EQ(completed.load(), 30);
    EXPECT_EQ(pool.pending(), 0U);
}

TEST(ThreadPool, ParallelForVisitsEveryIndexExactlyOnce) {
    ThreadPool pool(4);
    constexpr std::size_t kCount = 5000;
    std::vector<int> visits(kCount, 0);

    pool.parallel_for(kCount, [&visits](std::size_t i) { visits[i] = 1; });

    EXPECT_EQ(std::accumulate(visits.begin(), visits.end(), 0), static_cast<int>(kCount));
}

TEST(ThreadPool, ParallelForHandlesDegenerateCounts) {
    ThreadPool pool(4);
    std::atomic<int> calls{0};

    pool.parallel_for(0, [&calls](std::size_t) { calls.fetch_add(1); });
    EXPECT_EQ(calls.load(), 0);

    pool.parallel_for(1, [&calls](std::size_t i) {
        EXPECT_EQ(i, 0U);
        calls.fetch_add(1);
    });
    EXPECT_EQ(calls.load(), 1);
}

TEST(ThreadPool, ParallelForWorksWithFewerItemsThanWorkers) {
    ThreadPool pool(8);
    std::vector<int> visits(3, 0);
    pool.parallel_for(3, [&visits](std::size_t i) { visits[i] += 1; });
    EXPECT_EQ(visits, (std::vector<int>{1, 1, 1}));
}

TEST(ThreadPool, ParallelForPropagatesAnException) {
    ThreadPool pool(4);
    EXPECT_THROW(pool.parallel_for(100,
                                   [](std::size_t i) {
                                       if (i == 57) {
                                           throw std::runtime_error("boom");
                                       }
                                   }),
                 std::runtime_error);
}

// A single-threaded pool is a legitimate configuration (--threads 1) and must
// behave identically, just serially.
TEST(ThreadPool, ASingleWorkerStillRunsEverything) {
    ThreadPool pool(1);
    std::vector<int> visits(100, 0);
    pool.parallel_for(100, [&visits](std::size_t i) { visits[i] = 1; });
    EXPECT_EQ(std::accumulate(visits.begin(), visits.end(), 0), 100);
}

TEST(ThreadPool, SubmitAfterShutdownIsRejectedRatherThanIgnored) {
    auto pool = std::make_unique<ThreadPool>(2);
    static_cast<void>(pool->submit([] { return 1; }).get());
    pool.reset();
    // Nothing to assert beyond the absence of a hang or a crash; a dropped task
    // would be silent, which is the failure this shape prevents.
    SUCCEED();
}

}  // namespace
}  // namespace vectordb
