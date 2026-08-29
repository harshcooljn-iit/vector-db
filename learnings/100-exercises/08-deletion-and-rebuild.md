# Exercise 8 — Deletion and rebuild

🟡 Intermediate · [ADR-0011](../../docs/decisions/ADR-0011-tombstones.md)

## Goal

Measure what tombstones cost, and find the point where compaction is worth it.

## Background

Deleting a vector clears a liveness byte. The floats stay; the HNSW node stays
in the graph, still routing traffic, filtered out of results.

The claim is that this is cheap and safe — that a dead node still earns its keep
as a router. Test it.

## Setup

```sh
vectordb generate --output data.vecs --dimension 128 --count 50000 --seed 5
vectordb create t.vdb --dimension 128 --metric l2
vectordb import t.vdb --input data.vecs --start-id 0
```

## Part A: the cost curve

Delete 10%, 25%, 50%, 75%, 90% of the vectors, measuring after each: search
latency, recall against a freshly-built index over the survivors, and
`stats` memory.

**Predict the shape first.** Should latency rise linearly with the tombstone
ratio?

**Questions.** Where does recall start to suffer? Where does latency? Which
degrades first, and why?

## Part B: connectivity

At 90% deleted, the graph is mostly dead nodes.

**Questions.** Is it still connected? Instrument the search to count nodes
visited versus results returned. What is the ratio at 0% and at 90%? What does
that ratio *mean* for the tombstone argument?

## Part C: compaction

```sh
time vectordb compact t.vdb
```

**Questions.** How long does it take, and what is it doing? (Read `compact` in
`database.cpp`.) Compare with the original import time. Confirm that `VectorId`s
survived and that search still works.

## Part D: the threshold

`check` warns above a 50% tombstone ratio.

**Questions.** From your Part A data, is 50% the right threshold? Would you
automate compaction? What would the failure mode of automatic compaction be —
think about a long pause arriving unannounced.

## Part E: filter during, not after

`HnswIndex::search` filters tombstones **after** the traversal, on purpose:
skipping them during would cut their edges.

Change it to skip during traversal and measure recall at 50% deleted.

**Questions.** What happens, and does it match the ADR's prediction? Is the graph
still connected? This is the experiment the ADR is asserting without proof —
does it hold?

## Part F: the alternative

Sketch the design for physical deletion: find every node pointing at the victim,
repair those lists, handle the bridging case.

**Questions.** What data structure would "who points at me?" need, and what would
it cost? Is it cheaper than periodic compaction? Under what workload would it
win?

## Where to look

- `src/storage/vector_store.cpp` — `remove`, the tombstone
- `src/index/hnsw/hnsw_index.cpp` — `remove`, and the filter in `search`
- `src/db/database.cpp` — `compact`
- `tests/unit/index_hnsw_test.cpp` — `RecallSurvivesHeavyDeletion...`
