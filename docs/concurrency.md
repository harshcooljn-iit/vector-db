# Concurrency

What is safe to do at the same time, what is not, and why.

Implementation: `src/concurrency/thread_pool.cpp`, `src/db/concurrent_database.cpp`.
Decision record: [ADR-0013](decisions/ADR-0013-concurrency.md).

## The contract

| Operation | Concurrency |
|---|---|
| `search`, `get`, `contains`, `stats` | any number simultaneously |
| `insert`, `upsert`, `remove` | **exclusive** |
| `flush`, `rebuild_index`, `compact` | **exclusive** |

`Database` itself does **no locking**. It documents the contract; it does not
enforce it. `ConcurrentDatabase` wraps it in a `std::shared_mutex` and does.

That split is deliberate. A single-threaded caller — the CLI, a test, a batch
import — pays nothing for a lock it does not need, and a caller with its own
locking scheme is not forced to nest ours inside theirs.

## Why a reader-writer lock

A plain `std::mutex` would be correct and would serialise every search. This is
a read-mostly workload — that is the shape of a vector database — so
serialising reads discards the only easy parallelism available.

`std::shared_mutex` gives many readers or one writer. Readers do not block each
other; a writer waits for the readers to drain.

```
reader ────────────►
reader   ───────────────►      no waiting between readers
reader ──────►
writer            ·······──────►   waits, then has it alone
reader                    ·······──────►   waits for the writer
```

## What is not safe, and is not claimed

**Concurrent writers.** Insertion mutates the HNSW graph: it appends link
storage, rewrites neighbour lists, and can move the entry point. Making that
safe needs per-node locking with a careful acquisition order to avoid deadlock,
plus a scheme for reclaiming replaced neighbour lists while a reader may still
be traversing them.

Not implemented. Stated here rather than left to be discovered under load.

## The exception: `MetadataStore` locks itself

Every other class follows "reads are safe, writes need exclusive access, the
caller locks". `MetadataStore` does not: it is internally synchronised.

The reason is that its reads are not read-only. SQLite prepared statements are
compiled once and reused, and a `sqlite3_stmt` carries its own cursor and bound
parameters. Stepping one from two threads at once corrupts both — no amount of
caller-side reader/writer locking can help, because both threads are legitimate
*readers*.

This was found by a test, not by reasoning: `ConcurrentSearchesAgreeWith-
SequentialOnes` terminated with `bind integer failed: bad parameter or other
API misuse`, which is SQLite's way of saying a statement was used from two
places at once.

The lock is held only for the duration of one statement. Metadata is read `k`
times per query — once per returned result — not once per candidate, so this
serialises a small fraction of a search rather than its hot loop.

## The thread pool

```cpp
ThreadPool pool(8);
pool.parallel_for(queries.size(), [&](std::size_t i) { ... });
```

**Why pool at all?** Creating a thread costs 10–100 µs: a stack allocation, a
kernel task structure, a scheduling decision. A search takes tens of
microseconds. You would spend longer hiring the worker than doing the work.

Threads are also not free to *have* — a thousand concurrent queries would create
a thousand threads on eight cores, and the scheduler would thrash.

**Why block instead of spin?** A worker polling an empty queue burns a whole
core. On a laptop that also burns battery and pushes the machine into thermal
throttling, slowing down the threads that *are* working. Workers wait on a
condition variable, so the OS removes them from the run queue entirely.

**Why chunks in `parallel_for`?** Per-task overhead is a queue push, a mutex
acquisition and a `notify` — comparable to the cost of one short body. Handing
out contiguous ranges amortises that over many iterations.

### Details that matter

- `notify_all` on shutdown, not `notify_one`. Every worker is waiting and every
  one must wake to see the flag; waking one leaves the rest blocked and `join`
  never returns.
- **Predicate `wait`.** Spurious wakeups are real and permitted; a bare `wait()`
  can return with the queue still empty, and popping from it would be undefined.
- **Tasks run outside the lock.** Holding it during the task would serialise
  everything and make the pool an expensive way to run things one at a time.
- **The destructor drains.** A pool destroyed right after `submit` still runs
  the work rather than silently dropping something the caller was told was
  accepted.
- **Exceptions travel through the future.** A worker that lets one escape calls
  `std::terminate` and takes the process with it.
- **`parallel_for` must not be called from inside a task on the same pool.**
  Every worker would block waiting for work only a worker can run. There is no
  work-stealing and no deadlock detection — documented rather than guarded,
  because the check would cost something on every call.

## Batch search

Each worker takes its own shared lock rather than one being held across the
whole batch. Holding it outside would be marginally cheaper and would let a long
batch starve a writer indefinitely: most `shared_mutex` implementations only
admit a waiting writer at a moment when no reader holds the lock.

Each worker writes only `results[i]`. Distinct elements of a vector are distinct
objects, and concurrent writes to distinct objects are not a data race, so the
output needs no synchronisation at all.

## HNSW's search scratch

`HnswIndex::search` is `const` and needs an `O(N)` visited set plus several
heaps. Allocating those per query would dominate a search that touches a few
thousand nodes; making them members would be a data race the moment two threads
search at once.

They are `thread_local`. Each thread gets its own, with no locking and no
per-query allocation.

The cost, stated rather than hidden: the buffers live as long as the thread, and
a thread that searched a 10M-node index keeps a 40 MB visited set even if it
never searches again.

## Testing

Two layers, because stress tests alone are not enough.

**Stress tests** run eight threads searching while a writer inserts, and assert
that concurrent results are identical to sequential ones and that every returned
id resolves.

**ThreadSanitizer** (`cmake --preset tsan`) finds races that did not happen to
manifest. This matters here: a race in HNSW insertion might corrupt one
neighbour list once in ten thousand runs and surface months later as an
unexplained recall drop that no test reproduces. TSan finds it the first time.

The whole concurrency suite is clean under TSan, and CI runs the ASan preset on
every push.

## Measured

See [benchmark-results.md](benchmark-results.md) for batch-search throughput
against thread count on this machine. The short version is that scaling is good
up to the physical core count and flattens after it — the workload is
memory-bandwidth-bound, so hyperthreads have little left to exploit.
