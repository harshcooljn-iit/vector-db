# Atomics — and why they are not a faster mutex

🔴 Advanced

## What atomic means

An operation other threads cannot observe half-finished.

```cpp
int counter = 0;             // counter++ is load, add, store — interruptible
std::atomic<int> counter{0}; // counter++ is one indivisible operation
```

On most hardware this compiles to a single instruction (`lock xadd` on x86,
`ldaxr`/`stlxr` on ARM). No kernel involvement, no context switch.

## The temptation, and the trap

Atomics look like fast mutexes. They are not — they are a *different tool* that
solves a smaller problem.

**They protect one variable, not an invariant.**

```cpp
std::atomic<int> count;
std::vector<Item> items;

count.fetch_add(1);          // atomic
items.push_back(item);       // NOT atomic
                             // and the pair is not atomic together
```

Another thread can observe `count == 1` while `items` is empty. Each operation
is atomic; the *relationship between them* is not, and the relationship is what
you actually care about.

**When your invariant spans more than one variable, you need a mutex.**

## Where VectorDB uses them

Deliberately little:

| Use | Why an atomic is enough |
|---|---|
| test counters | a single counter, no invariant |
| `stop` flags in tests | one boolean, one writer |

And where it does **not**:

- `TopKCollector` — a heap has an invariant across many elements
- `HnswIndex` — the graph has an invariant across many nodes
- `VectorStore` — four parallel arrays that must agree

That restraint is the point.

## Memory ordering

This is where atomics get genuinely hard.

```cpp
counter.fetch_add(1, std::memory_order_relaxed);
flag.store(true, std::memory_order_release);
if (flag.load(std::memory_order_acquire)) { ... }
```

CPUs and compilers reorder memory operations. Memory ordering says how much
reordering is permitted around an atomic.

| Order | Guarantees |
|---|---|
| `relaxed` | atomicity only, no ordering |
| `acquire` / `release` | a release-store synchronises-with an acquire-load of the same variable |
| `seq_cst` | a single total order across all threads (the default) |

`seq_cst` is the default because it is the one people reason about correctly. The
weaker orders are faster and are where subtle bugs live.

**Advice**: use the default until a profiler tells you the atomic is a
bottleneck, and then read the memory model properly rather than guessing.

## The ABA problem

The classic reason lock-free is harder than it looks.

```
thread 1: reads head = A, prepares to CAS(A → B)
thread 2: pops A, pops B, pushes A back
thread 1: CAS succeeds — head is A again — but the list is now wrong
```

The pointer is the same; the *world* is not. Solutions (tagged pointers, hazard
pointers, epoch reclamation) are each a project.

This is a large part of why
[ADR-0013](../../docs/decisions/ADR-0013-concurrency.md) chose a `shared_mutex`:
a lock-free index needs a reclamation scheme to know when a replaced neighbour
list can be freed while a reader may still be walking it.

## When atomics genuinely win

- **A single counter** with no related state. Metrics, reference counts.
- **A flag** one thread sets and others poll.
- **Reference counting** — `shared_ptr` uses a relaxed increment and an
  acquire-release decrement.
- **Sequence locks** for a small, frequently-read, rarely-written value.

Notice the shape: **one variable, or one variable's worth of invariant.**

## `volatile` is not atomic

```cpp
volatile int counter;    // ❌ not thread-safe
```

`volatile` means "do not optimise away this access" — it is for
memory-mapped hardware registers. It provides **no** atomicity and **no**
ordering. Using it for threading is a decades-old misconception that is still
common.

The only legitimate use in this codebase is the benchmark's sink, which exists
to stop the compiler deleting a loop.

## Experiments

1. Increment a counter 10,000,000 times from 8 threads: unsynchronised, with a
   mutex, with `atomic<int>`, and with `atomic<int>` using `relaxed`. Compare
   correctness and time.
2. Make a `std::atomic<bool>` `volatile` instead and run under `tsan`.
3. Implement a lock-free stack with a CAS loop, then construct the ABA scenario
   by hand.
4. Replace `TopKCollector`'s heap with atomics and try to keep it correct. Notice
   where you get stuck — that is the invariant-spanning-variables problem.
5. Read the assembly for `fetch_add` with `relaxed` versus `seq_cst` on arm64.

## Where this lives in the code

- `src/concurrency/thread_pool.cpp` — mutexes, deliberately, not atomics
- `tests/integration/concurrent_database_test.cpp` — atomics for counters only
- `docs/decisions/ADR-0013-concurrency.md` — why not lock-free

---

Next: [../70-performance/01-cache-locality.md](../70-performance/01-cache-locality.md)
