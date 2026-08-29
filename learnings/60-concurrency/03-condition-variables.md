# Condition variables — waiting without burning a core

🟡 Intermediate

## The problem

A worker with nothing to do could poll:

```cpp
while (true) {
    if (!queue.empty()) { ... }     // ☠️
}
```

That burns a **full core** doing nothing. On a laptop it also burns battery and
pushes the machine into thermal throttling, which slows down the threads that
*are* working. You have made things worse by trying harder.

You want the thread to **sleep** until there is work, and cost nothing while
asleep.

## The mechanism

```cpp
std::unique_lock<std::mutex> lock(mutex_);
work_available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
```

`wait` **atomically** releases the mutex and parks the thread. The scheduler
removes it from the run queue entirely.

`notify_one` wakes it; it reacquires the mutex before `wait` returns.

## Why the atomicity matters

If releasing the mutex and sleeping were separate steps, this could happen:

```
worker: sees empty queue
worker: releases mutex
                            producer: pushes work
                            producer: notify_one   ← nobody is waiting yet
worker: sleeps                                     ← forever
```

That is the **lost wakeup**, and eliminating it is precisely what condition
variables exist for.

## Always use the predicate form

```cpp
cv.wait(lock, predicate);            // ✅
while (!predicate()) cv.wait(lock);  // identical — this is what it expands to
cv.wait(lock);                       // ❌
```

`wait` may return **spuriously** — with no notify at all. The standard permits
it, and on some platforms it genuinely happens (a signal, or an implementation
detail of the futex).

Popping from an empty queue because of a spurious wakeup is undefined behaviour,
and it will be rare enough to reach production and never reproduce.

## `notify_one` vs `notify_all`

```cpp
work_available_.notify_one();     // new work: one worker suffices

{ std::lock_guard lock(mutex_); stopping_ = true; }
work_available_.notify_all();     // shutdown: EVERY worker must wake
```

At shutdown, every worker is parked and every one must observe the flag and
return. `notify_one` would leave the rest blocked forever and `join()` would
never come back — a hang at exit, which is a miserable bug to chase.

The rule: **`notify_one` when any one waiter can handle it; `notify_all` when the
condition concerns all of them.**

## Notify with or without the lock?

Both are correct.

Holding it means the woken thread immediately blocks on the mutex you still hold
(a "hurry up and wait"). Releasing first avoids that, at the cost of a slightly
wider window.

VectorDB releases first for `notify_one` in `submit`. Either is fine; the
important thing is that the *state change* happens under the lock.

## Two condition variables, two questions

```cpp
std::condition_variable work_available_;   // workers: "is there a task?"
std::condition_variable all_idle_;         // wait_idle(): "is everything done?"
```

Different predicates, different waiters. Sharing one would wake threads that
cannot make progress — harmless but wasteful, and confusing to read.

## The full worker loop

```cpp
while (true) {
    std::function<void()> task;
    {
        std::unique_lock lock(mutex_);
        work_available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty()) return;      // ← note the &&
        task = std::move(tasks_.front());
        tasks_.pop();
        ++active_;
    }
    task();                                            // outside the lock
    {
        std::lock_guard lock(mutex_);
        --active_;
        if (tasks_.empty() && active_ == 0) all_idle_.notify_all();
    }
}
```

Every line is load-bearing:

- **`&&` in the exit test** — drain before exiting, so a pool destroyed right
  after `submit` still runs the work.
- **`task()` outside the lock** — or the pool serialises everything.
- **`active_`** — `wait_idle` must wait for *running* tasks, not just queued
  ones.

## Experiments

1. Replace the wait with a spin loop. Watch a core pin at 100% while idle, and
   (on a laptop) watch throughput fall as the machine heats.
2. Change `notify_all` to `notify_one` in the destructor. Observe the hang.
3. Replace `wait(lock, pred)` with a bare `wait(lock)` and hammer the pool.
   Depending on your platform this may survive for a long time before it does
   not.
4. Remove `active_` and see `wait_idle` return while tasks are still running.
5. Change `stopping_ && tasks_.empty()` to just `stopping_` and count how many
   tasks get dropped on shutdown.

## Where this lives in the code

- `src/concurrency/thread_pool.cpp` — `worker_loop`, `wait_idle`, the destructor
- `tests/unit/concurrency_thread_pool_test.cpp` — `RunsQueuedWorkBeforeShuttingDown`,
  `WaitIdleBlocksUntilEverythingHasFinished`

---

Next: [04-thread-pools.md](04-thread-pools.md)
