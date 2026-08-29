# Exercise 3 — The efSearch curve

🟡 Intermediate · [HNSW algorithm](../40-search-indexes/04-hnsw-algorithm.md)

## Goal

Produce the recall/latency curve for your own machine and data, and find the
knee — the point past which extra latency buys nothing.

## Background

`efSearch` is the only HNSW parameter you can change **at runtime**. `M` and
`efConstruction` are baked into the graph.

Measured on an M1 at N = 50,000, D = 128, M = 16:

| efSearch | p50 (µs) | recall@10 |
|---|---|---|
| 10 | 20.0 | 0.9590 |
| 20 | 25.0 | 0.9880 |
| 100 | 46.6 | 1.0000 |
| 400 | 149.1 | 1.0000 |

The default is 64. Look at that table and form an opinion about the default.

## Part A: reproduce it

```sh
./build/release/bin/vectordb-bench hnsw
```

**Questions.** Do you get the same shape? The same absolute numbers? If not, what
differs about your machine — cores, memory bandwidth, compiler?

## Part B: find your knee

Extend the driver to sweep `efSearch` from 1 to 1000 and plot recall against p50
latency.

**Questions.** Where does recall reach 0.99? 0.999? What does the last 1% of
recall cost, as a multiple of the latency at 0.99? Is the default of 64 too high
for this data?

## Part C: make it hard

At N = 50,000 recall hits 1.000 and stays there — HNSW is effectively exact, so
recall stops being a useful signal.

Make it informative again by starving the graph:

```sh
vectordb create hard.vdb --dimension 128 --m 4 --ef-construction 10
```

Or raise N to 500,000, or D to 1536.

**Questions.** Which change produces the most interesting curve? What does that
tell you about when ANN is worth using at all?

## Part D: the `k` interaction

`HnswIndex::search` raises `ef` to `k` when a caller asks for more results than
the candidate width.

Remove that clamp and search with `k = 100`, `efSearch = 10`.

**Questions.** How many results come back? Was the caller told? Why is silently
returning fewer results than requested worse than an error?

## Part E: recall on the results that matter

Recall@10 counts all ten positions equally. Users notice position 1 far more.

Measure recall@1 as well.

**Questions.** How do the two curves differ? Which would you optimise for in a
search UI? In a deduplication pipeline?

## Where to look

- `src/index/hnsw/hnsw_index.cpp` — `search`, and the `ef = max(k, ef)` clamp
- `benchmarks/main.cpp` — `benchmark_hnsw`
- `tests/support/recall.hpp` — `recall_at_k`
- `docs/benchmark-results.md` — the reference table
