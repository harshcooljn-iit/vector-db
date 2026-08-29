# Threads — the mental model

🟢 Foundational

## What a thread is

A thread is an independent path of execution **inside one process**. Threads in a
process share:

- the heap
- global and static variables
- file descriptors
- the memory mapping

and each has its own:

- stack (typically 512 KB – 8 MB)
- registers, including the instruction pointer
- `thread_local` storage

**Shared memory is the point and the problem.** Two threads reading the same
array is free. Two threads writing it is a bug unless you did something about it.

## Concurrency is not parallelism

**Concurrency** is structure: several things in progress at once. **Parallelism**
is execution: several things happening at the same instant.

One core can be concurrent (interleaving) but not parallel. Eight cores can be
both.

VectorDB wants parallelism — eight searches actually running at once — and gets
it through concurrency: a pool of threads over a queue of queries.

## Threads are expensive to create

Constructing a `std::thread` costs 10–100 µs: a stack allocation, a kernel task
structure, a scheduling decision. A search takes tens of microseconds.

Hence [thread pools](04-thread-pools.md). Threads are a resource to be pooled,
like database connections.

## Threads are expensive to *have*

Every runnable thread competes for a core. A thousand threads on eight cores
means the scheduler round-robins, every switch flushes cache, and throughput
*falls* as you add work. This is **oversubscription**.

Rule of thumb: as many threads as cores for CPU-bound work; more only when
threads block on I/O.

## Where VectorDB uses them

| Where | Why |
|---|---|
| `ThreadPool` | one pool, sized to `hardware_threads()` |
| `batch_search` | many queries across the pool |
| never in `HnswIndex::add` | insertion mutates the graph; not thread-safe |

Note what is *not* threaded. A single search is not parallelised internally —
HNSW's traversal is inherently sequential (each step depends on the last), so
parallelism lives at the query level.

## `hardware_concurrency` can return 0

```cpp
std::size_t hardware_threads() noexcept {
    const unsigned int reported = std::thread::hardware_concurrency();
    return reported == 0 ? 1 : static_cast<std::size_t>(reported);
}
```

The standard permits 0 for "cannot determine", and a pool of zero threads would
silently never run anything. There is a test.

## Joining is not optional

```cpp
std::thread t([]{ ... });
// destructor runs without join() or detach() → std::terminate
```

`std::jthread` (C++20) joins in its destructor and supports cooperative
cancellation. Worth reaching for in new code; `ThreadPool` predates the need and
joins explicitly.

## Every thread needs its own stack

Which means `thread_local` is not free — `HnswIndex`'s search scratch includes an
`O(N)` visited set, and each searching thread keeps one for the life of the
thread. Stated in the header rather than discovered under memory pressure.

## Experiments

1. Time creating and joining 10,000 threads versus submitting 10,000 tasks to a
   pool.
2. Run the batch benchmark with 1, 4, 8, 32 and 128 threads. Find where
   oversubscription starts to hurt.
3. Print `std::this_thread::get_id()` from inside `parallel_for` and confirm the
   work really spreads.
4. Create a `std::thread` and let it go out of scope without joining. Read the
   `terminate` message.

## Where this lives in the code

- `src/concurrency/thread_pool.cpp` — `hardware_threads`, worker lifecycle
- `src/index/hnsw/hnsw_index.cpp` — `thread_local` scratch
- `tests/unit/concurrency_thread_pool_test.cpp`

---

Next: [02-mutexes.md](02-mutexes.md)
