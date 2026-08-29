# Exercise 7 — Thread scaling

🟡 Intermediate · [reader-writer concurrency](../60-concurrency/05-reader-writer-concurrency.md)

## Goal

Find where adding threads stops helping, and identify what is limiting you.

## Background

Measured on an M1 (4 performance + 4 efficiency cores), N = 50,000, D = 128:

| threads | queries/s | scaling |
|---|---|---|
| 1 | 51,368 | 1.00× |
| 2 | 98,875 | 1.92× |
| 4 | 187,410 | 3.65× |
| 8 | 214,987 | 4.19× |

Near-linear to 4, then flat.

## Part A: reproduce

```sh
./build/release/bin/vectordb-bench batch
```

**Questions.** Same shape? Where is your knee? How many physical cores do you
have, and does the knee match?

## Part B: two candidate explanations

The flattening has two plausible causes, and they compound:

1. The M1's efficiency cores are slower than its performance cores.
2. The workload is memory-bound, so extra threads contend for bandwidth.

Design an experiment that distinguishes them.

Hint: a *compute-bound* workload with the same threading structure would isolate
(1). `vectordb-bench distance` is compute-bound — can you make a threaded
version?

**Questions.** Which explanation dominates on your machine? On a machine with
homogeneous cores, what would you expect?

## Part C: oversubscribe

Run with 16, 32 and 64 threads.

**Questions.** Does throughput fall? What is the mechanism — context switches,
cache pressure, scheduler overhead? Where would you cap the pool by default?

## Part D: the metadata trap

Re-run the batch benchmark with `include_metadata = true`.

**Questions.** What happens to scaling? Why? (`docs/benchmark-results.md` §4 has
the story, but predict before reading.) What would it take to fix properly, given
that `MetadataStore` must serialise access to its prepared statements?

## Part E: writers

`ConcurrentDatabase` allows one writer at a time. Start a long batch search and,
from another thread, insert.

**Questions.** How long does the writer wait? Now change `batch_search` to hold
one shared lock across the whole batch instead of per query, and measure again.
Which is better, and for whom?

## Part F: measure the lock itself

Time `shared_lock` acquisition with 1 thread and with 8 threads doing nothing
else.

**Questions.** What is the cost, and where does it come from? (Every
`shared_lock` atomically increments a shared counter, so that cache line bounces
between cores.) At what query rate would this become the bottleneck?

## Where to look

- `src/db/concurrent_database.cpp` — the locking, and the per-query lock comment
- `src/concurrency/thread_pool.cpp` — `parallel_for`
- `docs/concurrency.md`, `docs/benchmark-results.md` §4
