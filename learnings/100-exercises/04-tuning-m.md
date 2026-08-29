# Exercise 4 — Tuning M

🟡 Intermediate · [HNSW intuition](../40-search-indexes/03-hnsw-intuition.md)

## Goal

Find your own M, and understand what you are trading.

## Background

`M` is neighbours per node above layer 0; layer 0 gets `2M`. It is the dominant
memory parameter and, unlike `efSearch`, changing it requires a rebuild.

Measured overhead at M = 16: **145 bytes/vector**, independent of dimension.

## Part A: sweep it

For M ∈ {4, 8, 16, 32, 64} at N = 50,000, D = 128, record build time,
`index_bytes()`, recall@10 at a **fixed** efSearch, and p50 latency.

```sh
vectordb create m4.vdb --dimension 128 --m 4 --ef-construction 200
```

**Questions.** Which of the four moves fastest with M? Where does recall stop
improving? Where does memory become uncomfortable?

## Part B: the fair comparison

Part A held `efSearch` fixed, which is not quite fair — a bigger graph might not
need as much search effort.

Redo it, tuning `efSearch` per M to hit **recall ≥ 0.99**, and compare the
latency at that point.

**Questions.** Does the ranking change? Which M gives the best latency at fixed
recall? Is that the same M you would pick from Part A?

## Part C: the low end

M = 2 and M = 3 are permitted (validation rejects M < 2).

**Questions.** What happens to recall? To connectivity — are there nodes with no
usable edges? Should `HnswConfig::validate` reject values below, say, 5? What
would you lose by forbidding them?

## Part D: layer 0's extra budget

Layer 0 gets `2M` by convention. Change `max_m0()` to return `m` (equal budget)
and to `4 * m`.

**Questions.** Which matters more — layer 0's density or the upper layers'? Why
would that be, given where search spends its time? Is the factor of 2 the right
default, or just the paper's?

## Part E: memory arithmetic

`docs/hnsw.md` gives:

```
bytes ≈ N × (2M + 1) × 4  +  upper layers  +  N × 5
```

**Questions.** Compute the prediction for M = 16, N = 50,000. Compare with the
measured 145 bytes/vector. Where does the difference come from? (Look at
`index_bytes()` — it reports `capacity()`, not `size()`.)

## Part F: pick one

You are storing 5,000,000 documents at D = 768 on a machine with 32 GB.

**Questions.** What is the vector payload alone? What M can you afford? What
recall does that give you, extrapolating from your measurements? Would you
reach for quantization instead — and what would that change?

## Where to look

- `include/vectordb/index/hnsw_config.hpp` — the parameters and their trades
- `src/index/hnsw/hnsw_index.cpp` — `index_bytes`, the link layouts
- `docs/hnsw.md` — the memory formula
