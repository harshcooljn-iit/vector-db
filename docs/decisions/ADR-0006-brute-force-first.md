# ADR-0006 — Build brute force first, and keep it forever

**Status:** Accepted

## Decision

A complete, exact brute-force index is implemented and tested before any HNSW
code is written. It is not scaffolding: it ships, it is selectable
(`--index=brute_force`), and it is the ground truth in every HNSW test.

## Context

HNSW is approximate. It can return the wrong answer and still look completely
healthy: results come back, distances are plausible, latency is good. There is
no internal signal that says "that was wrong".

So the question "is my HNSW implementation correct?" is unanswerable in
isolation. It only becomes answerable relative to something exact.

## Options considered

1. **Implement HNSW directly**, validate against hand-computed expectations for
   tiny datasets.
2. **Implement HNSW**, validate against an external library.
3. **Implement brute force first**, validate HNSW against it. (chosen)

Option 1 does not scale past about a dozen vectors, and the interesting HNSW
bugs — a pruning rule that disconnects a region, an entry point that never
updates — only appear at thousands. Option 2 reintroduces the dependency that
[ADR-0001](ADR-0001-implement-algorithms-ourselves.md) exists to avoid, and
would validate our HNSW against *their* HNSW rather than against the truth.

## Chosen approach

Option 3.

```
build brute-force index  ─┐
build HNSW index         ─┤── same dataset, same queries
                          ├── compare result sets
                          └── recall@k = |ours ∩ exact| / k
```

## Why

- **Brute force is correct by construction.** It computes every distance and
  keeps the smallest k. There is nowhere for a subtle bug to hide, which makes
  it a trustworthy oracle.
- **It turns "is HNSW right?" into a number.** `recall@10 = 0.98` is a fact you
  can put in a test with a threshold, track across commits, and trade against
  latency by turning `efSearch`.
- **It is genuinely useful.** For small collections, exact search is *faster*
  than HNSW — no graph to traverse, no index to build or persist. A database of
  10,000 vectors should not pay for HNSW.
- **It builds the shared machinery first.** Distance kernels, top-k selection,
  the index interface, filtering, the storage layer — brute force exercises all
  of it. When HNSW arrives, the only new thing is the graph.

## Trade-offs

| Cost | Benefit |
|---|---|
| Two index implementations to maintain | Approximate search has a correctness measure |
| Recall tests are slow (they run both) | Small databases get the faster option |
| Delays HNSW by a phase | Everything HNSW needs is already tested when it starts |

Recall tests being slow is managed by keeping them at modest N with fixed seeds,
and by keeping wall-clock assertions out of them entirely.

## Consequences

- The index interface must be designed so both implementations satisfy it
  without either distorting it.
- Recall tests use fixed random seeds so a failure is reproducible. They assert
  a *threshold*, not an exact value — demanding bit-identical ordering from an
  approximate algorithm is how you get a permanently red, permanently ignored
  test.
- Brute force is the fallback when an index file fails validation: the vector
  store is authoritative, so exact search always works even when the graph does
  not.

## Alternatives for future versions

Sampling-based recall estimation (run brute force on a random subset of
queries) would let recall be checked on much larger datasets in reasonable
time. Useful once N is large enough that full brute force in a test is
prohibitive.
