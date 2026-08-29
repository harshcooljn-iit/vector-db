# Exercise 10 — Bad benchmark data

🔴 Advanced · [ANN note](../40-search-indexes/02-approximate-nearest-neighbour.md)

## Goal

Reproduce the bug that made this project's HNSW look broken when it was not, and
learn to recognise the failure mode.

## The story

Early in development, HNSW measured **recall@10 = 0.752** at M = 16,
efConstruction = 200, efSearch = 100 — parameters that should be near-exact.

The graph was correct. The *benchmark data* was wrong: `generate_queries` derived
a fresh seed for everything, including the cluster centroids, so queries came
from a different cluster layout and landed in the empty space between the
dataset's clusters.

## Part A: reproduce it

In `src/util/dataset.cpp`, make `generate_queries` derive both seeds again:

```cpp
std::uint64_t derived = spec.seed;
query_spec.seed = splitmix64(derived);          // ← the bug
query_spec.sample_seed = splitmix64(derived);
```

Rebuild and run the recall tests.

**Questions.** What recall do you get? Do the *structural* HNSW tests still pass?
What does that tell you about which tests would have caught this and which would
not?

## Part B: measure the concentration

Write a program that, for a query and a dataset, reports the distance to the 1st,
10th, 100th and median nearest neighbours.

Run it with buggy queries and with correct ones.

Expected shape (measured at D = 64, N = 5,000):

| | nearest | 10th | median | ratio 10th/1st |
|---|---|---|---|---|
| buggy queries | 103.7 | 106.6 | 154.6 | **1.03** |
| correct queries | 1.68 | 2.09 | 133.7 | **1.24** |

**Questions.** With a ratio of 1.03, how meaningful is "the true top 10"? If
hundreds of points are within 3% of each other, what is recall actually
measuring?

## Part C: the curse, directly

For D = 2, 10, 50, 200, 768, generate 10,000 **uniform** points and compute

```
(distance to furthest − distance to nearest) / distance to nearest
```

averaged over 100 queries.

**Questions.** How does it behave as D grows? At what dimension does "nearest
neighbour" stop being a meaningful concept for uniform data? Why do real
embeddings escape this?

## Part D: kind by kind

Run the recall suite with `kUniform`, `kGaussian` and `kClustered`.

**Questions.** How much does recall differ? If a paper reports ANN recall without
saying what data it used, how much do you learn from the number?

## Part E: sweep the difficulty

`cluster_stddev` controls cluster tightness. Sweep it from 0.05 to 2.0 and plot
recall at a fixed `efSearch`.

**Questions.** Where does it start to hurt? Where does clustered data become
indistinguishable from Gaussian? Which value is closest to real text embeddings —
and how would you find out?

## Part F: a test that would have caught it

The recall test at the time asserted `recall > 0.95`. The bug produced 0.752, so
it *did* fail — but only after a long investigation into the index.

Design a test that would have pointed at the **data** instead.

Hint: `util_dataset_test.cpp` has
`ClusteredDataHasMuchCloserNeighboursThanUniformData`, which was written after
this bug. Is it sufficient? What would you add?

## Then put it back

```sh
git checkout src/util/dataset.cpp
```

## Where to look

- `src/util/dataset.cpp` — `generate_queries`, with the numbers in a comment
- `git log --grep="cluster structure"` — the commit, with the full story
- `docs/benchmark-results.md` — "Why the data matters"
- `tests/unit/util_dataset_test.cpp`
