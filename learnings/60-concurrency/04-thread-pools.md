# Thread pools — why you don't create a thread per task

🟡 Intermediate · pairs with `src/concurrency/thread_pool.cpp`

## The naive version

```cpp
for (auto& query : queries) {
    std::thread([&] { search(query); }).detach();
}
```

Works. Then you measure it and discover the threads cost more than the work.

## What creating a thread actually costs

A `std::thread` is a **kernel** object. Constructing one means:

1. allocating a stack — commonly 512 KB to 8 MB of *address space*, of which the
   pages are committed lazily;
2. building a kernel task structure;
3. entering the scheduler's run queue;
4. a context switch to get it going.

Roughly **10–100 microseconds**. A vector search takes tens of microseconds. You
would spend longer hiring the worker than doing the job.

And threads are not free to *have*, either. A thousand concurrent queries means
a thousand threads on eight cores: the scheduler round-robins between them,
every switch flushes cache, and everything slows down together. This is
**oversubscription**, and its symptom is throughput falling as you add work.

## The pool

Create N threads once. Feed them from a queue.

```
        submit()                  workers
   ┌──────────────┐         ┌────────────────┐
   │ task │ task  │ ──────► │ thread 0  ─── running
   │ task │ task  │         │ thread 1  ─── running
   └──────────────┘         │ thread 2  ─── waiting on the condition variable
      the queue             └────────────────┘
```

N is the core count, so the machine is busy but not oversubscribed. **Threads
are a resource to be pooled**, exactly like database connections or file
handles.

## Blocking, not spinning

An idle worker could poll:

```cpp
while (true) {
    if (!queue.empty()) { ... }     // DON'T
}
```

That burns a full core doing nothing. On a laptop it also burns battery and
heats the machine into thermal throttling — which slows down the threads that
*are* working. You have made things worse by trying harder.

A **condition variable** hands the problem to the OS:

```cpp
std::unique_lock lock(mutex_);
work_available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
```

`wait` atomically releases the mutex and parks the thread — the scheduler
removes it from the run queue entirely, consuming nothing. `notify_one` wakes it
and it reacquires the mutex before returning.

The atomicity matters. If releasing and sleeping were separate steps, a notify
arriving in between would be missed and the thread would sleep forever. That is
the **lost wakeup** problem, and it is exactly what condition variables exist to
solve.

### Why the predicate form

```cpp
cv.wait(lock, predicate);        // ✅
while (!predicate()) cv.wait(lock);   // identical, and what it expands to

cv.wait(lock);                   // ❌
```

`wait` may return **spuriously** — with no notify at all. The standard permits
it, and on some platforms it genuinely happens. Popping from an empty queue
because of a spurious wakeup is undefined behaviour, and it will be rare enough
to reach production.

Always test the condition in a loop. The predicate overload *is* that loop.

## Details that bite

### `notify_all` on shutdown, not `notify_one`

```cpp
{ std::lock_guard lock(mutex_); stopping_ = true; }
work_available_.notify_all();
```

Every worker is parked, and every one must wake to observe the flag and return.
Waking one leaves seven blocked forever, and `join()` never comes back — a hang
at exit, which is a miserable bug to chase.

### Drain before exiting

```cpp
if (stopping_ && tasks_.empty()) return;
```

Note the `&&`. A pool destroyed immediately after `submit` still runs the queued
work. Exiting on `stopping_` alone would silently discard tasks the caller was
told had been accepted.

### Run tasks outside the lock

```cpp
{ std::unique_lock lock(mutex_); task = std::move(tasks_.front()); tasks_.pop(); }
task();     // ← lock released
```

Holding the mutex during the task would serialise everything and make the pool
an elaborate way to run things one at a time. It is an easy mistake precisely
because it still *works*.

### Exceptions must not escape a worker

An exception leaving a thread's entry function calls `std::terminate` — the
whole process dies, and the stack trace points at the thread, not the bug.

`std::packaged_task` captures it into the future instead, so it resurfaces at
`future.get()` in the calling thread. There is a test that a failing task does
not take the pool down and that the pool still works afterwards.

### `shared_ptr<packaged_task>`

```cpp
auto packaged = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(task));
tasks_.emplace([packaged] { (*packaged)(); });
```

`std::function` requires a **copyable** target; `packaged_task` is move-only.
The `shared_ptr` makes the lambda copyable. Slightly wasteful, and the standard
alternative (a move-only function wrapper) only arrives with C++23's
`std::move_only_function`.

## `parallel_for`, and why it chunks

```cpp
pool.parallel_for(1000, [&](std::size_t i) { results[i] = search(queries[i]); });
```

One task per index would be 1000 queue pushes, 1000 mutex acquisitions and 1000
notifies. Each of those is comparable to the cost of a short body — you would
spend as much on bookkeeping as on work.

Instead, `chunks = min(count, workers)` contiguous ranges. Eight tasks instead
of a thousand.

Two smaller decisions:

- **Below the threshold, run inline.** For `count == 1`, or a pool with no
  workers, synchronisation costs more than the work. The caller should not have
  to decide that.
- **`future.get()`, not `wait()`.** `get` rethrows, so an exception in any chunk
  reaches the caller. `wait` would swallow it.

### The deadlock this API can create

**Never call `parallel_for` from inside a task on the same pool.** The outer
task occupies a worker and blocks waiting for inner tasks, which need a worker
to run. With eight nested calls, all eight workers block and nothing ever
progresses.

Real pools solve this with **work stealing** — a blocked worker runs pending
tasks itself instead of sleeping. Ours does not, and does not detect the
deadlock either. That is documented rather than guarded, because the check would
cost something on every call to prevent a mistake the API shape already
discourages.

## Where the results go

```cpp
std::vector<std::vector<QueryResult>> results(queries.size());
pool.parallel_for(queries.size(), [&](std::size_t i) {
    results[i] = database_->search(queries[i], options);   // no lock!
});
```

No synchronisation on `results`, and that is correct. **Distinct elements of a
vector are distinct objects**, and concurrent writes to distinct objects are not
a data race. The vector is pre-sized, so nothing reallocates.

The one exception you must know: `std::vector<bool>` is bit-packed, so two
"distinct" elements can share a byte, and concurrent writes to them *are* a
race. It is the standard library's most notorious wart.

## Experiments

1. Time 10,000 trivial tasks via `std::thread` per task versus a pool. Then try
   10,000 tasks that each take 10 ms and watch the difference shrink — you have
   found where thread creation stops mattering.
2. Replace the condition variable with a spin loop. Watch a core pin at 100%
   while idle, and (on a laptop) watch throughput *fall* as the machine heats.
3. Change `notify_all` to `notify_one` in the destructor. Observe the hang.
4. Replace `wait(lock, predicate)` with a bare `wait(lock)` and run under load.
   Depending on your platform this may work for weeks before it does not.
5. Create a pool with 1, 2, 4, 8, 16 threads and measure batch-search
   throughput. Find the knee, and compare it with your core count.
6. Call `parallel_for` from inside a task. Watch it hang, then write down why.

## Where this lives in the code

- `include/vectordb/concurrency/thread_pool.hpp` — the interface, and the
  deadlock warning on `parallel_for`
- `src/concurrency/thread_pool.cpp` — `worker_loop`, the shutdown handshake,
  chunked `parallel_for`
- `src/db/concurrent_database.cpp` — `batch_search`, and why the results need no
  lock
- `tests/unit/concurrency_thread_pool_test.cpp` — every hazard above has a test
- `docs/concurrency.md` — the contract

---

Next: [05-reader-writer-concurrency.md](05-reader-writer-concurrency.md)
