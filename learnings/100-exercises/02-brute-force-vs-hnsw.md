# Exercise 2 — Brute force vs HNSW

🟢 Foundational · [ADR-0006](../../docs/decisions/ADR-0006-brute-force-first.md)

## Goal

Find the N at which HNSW starts paying for itself on *your* machine, rather than
taking "HNSW is faster" on faith.

## Background

HNSW is asymptotically better and has real fixed costs: build time, ~145
bytes/vector of graph, a file to persist and validate, and pointer-chasing that
does not prefetch. Brute force has none of those and streams memory perfectly.

So there is a crossover. Where?

## Part A: build both

```sh
vectordb generate --output data.vecs --dimension 128 --count 20000 --seed 42
vectordb create exact.vdb --dimension 128 --index brute_force
vectordb create ann.vdb   --dimension 128 --index hnsw
time vectordb import exact.vdb --input data.vecs
time vectordb import ann.vdb   --input data.vecs
```

**Questions.** How much longer does the HNSW import take? Where does that time
go — is inserting into the graph roughly as expensive as searching it?

## Part B: query both

```sh
vectordb generate --output q.vecs --dimension 128 --count 1 --seed 42 --sample-seed 999
vectordb search exact.vdb --query-file q.vecs --k 10 --time
vectordb search ann.vdb   --query-file q.vecs --k 10 --time
```

**Questions.** Are the result *lists* the same? If they differ, at which
positions and by how much in score? Would a user notice?

## Part C: the crossover

Repeat at N = 100, 1,000, 10,000, 100,000. Plot both latencies against N on a
log-log axis.

**Predict the shape first**: what slope should brute force have? What slope
should HNSW have?

**Questions.** Where do the lines cross? Now add the build time, amortised over
100 queries, then over 1,000,000. How does the crossover move?

## Part D: dimension

Repeat at D = 32, 128, 768 with N fixed at 20,000.

**Questions.** Does the crossover depend on D? Should it? (Think about what each
algorithm does per candidate versus how many candidates it examines.)

## Part E: memory

```sh
vectordb stats exact.vdb
vectordb stats ann.vdb
```

**Questions.** What is HNSW's index overhead per vector? Does it change with D?
Explain why from the formula in `docs/hnsw.md`. At which dimension does the graph
stop being a significant fraction of total memory?

## Part F: the oracle argument

Delete `BruteForceIndex` entirely and try to build.

**Questions.** What breaks? How would you now measure whether HNSW is correct?
Would you accept a system whose only correctness evidence was "it returns
plausible results"?

## Where to look

- `src/index/brute_force/brute_force_index.cpp` — and its class comment on why it stays
- `docs/benchmark-results.md` — sections 2 and 3
- `benchmarks/main.cpp` — `benchmark_brute_force`, `benchmark_hnsw`
