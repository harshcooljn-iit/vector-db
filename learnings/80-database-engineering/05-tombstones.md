# Tombstones — deleting without deleting

🟡 Intermediate · [ADR-0011](../../docs/decisions/ADR-0011-tombstones.md)

## What deletion actually does

```cpp
bool VectorStore::remove(VectorId id) {
    live_[slot] = 0;              // a byte
    id_to_local_.erase(id);       // forget the id
    --live_count_;
    return true;                  // the floats stay exactly where they were
}
```

`O(1)`. Nothing is moved, nothing is freed, no neighbour list is touched.

## Why not remove it properly?

For the **vector store**, physical removal means either moving every subsequent
vector (999,995 of them, to delete slot 5 of a million) or leaving a hole that
every future scan must special-case.

For the **graph**, it is worse. You would have to:

1. find every node listing the victim as a neighbour — the graph stores no
   reverse index, so that is a full scan;
2. repair each list, possibly re-running neighbour selection;
3. renumber, if slots compact, invalidating every stored edge;
4. and accept that the victim may have been **the only bridge between two
   regions**.

Point 4 is the dangerous one. Cutting a bridging node can disconnect part of the
graph — silently. Nothing errors; recall simply collapses for whatever ends up on
the wrong side, for queries you have not run yet.

## The trade

| Cost | Benefit |
|---|---|
| Deleted vectors still occupy memory and disk | deletion is `O(1)` |
| The traversal still visits them | the graph cannot become disconnected |
| Search over-fetches slightly to fill k | no neighbour lists to repair |
| Compaction is a separate, explicit step | deletion cannot corrupt the index |

## Filter *after* the traversal, not during

```cpp
search_layer(query, ..., found);          // traverse everything, tombstones included
for (const Candidate& c : found) {
    if (accessor_.live(c.local_id)) results.push_back(c);
}
```

This is the detail that makes the whole scheme work. A tombstoned node keeps
**routing** traffic — its edges are still the shortest path between the regions
it bridged. Skipping it during traversal would cut those edges and reintroduce
exactly the disconnection problem tombstones exist to avoid.

There is a test that deletes half the vectors from a 3,000-vector index and
confirms recall stays above 0.85. That is the evidence a dead node still earns
its keep.

## What tombstones force elsewhere

**Two counts.** `slot_count` and `live_count` are different numbers, both
persisted, and every layer that walks slots must know the difference.

**Two id spaces.** Compaction renumbers `LocalId`s, so user-visible `VectorId`s
must be separate — [ADR-0003](../../docs/decisions/ADR-0003-id-model.md) is what
makes compaction possible at all.

**Persisted liveness.** The vector file stores the liveness array. A reload that
resurrected deleted vectors would be a data-integrity bug; there is a test named
for it.

**A visible ratio.** `stats` reports it, and `check` warns above 50% with the
command that fixes it.

## Compaction

```sh
vectordb compact mydb
```

Rewrites the store with only live slots and rebuilds the graph. `VectorId`s are
untouched.

Deliberately **manual**. Automatic compaction above a threshold would be
convenient and would mean an unannounced multi-second pause arriving in the
middle of a workload. If it were automated, it should be incremental and
cancellable.

## The pattern, generalised

Tombstones are everywhere in storage systems:

| System | Tombstone |
|---|---|
| LSM trees (RocksDB, Cassandra) | a delete marker, merged away at compaction |
| Postgres MVCC | dead tuples, collected by `VACUUM` |
| Filesystems | freed blocks, reclaimed lazily |
| Garbage collectors | unreachable objects, collected later |

The shape is always the same: **make the frequent operation cheap by deferring
the expensive part, and provide an explicit way to pay it later.**

The corollary is also always the same: you now have two states (logical and
physical) that can drift, and you need a way to observe the drift and a way to
close it. `stats` is the first; `compact` is the second.

## Experiments

1. Work through [Exercise 8](../100-exercises/08-deletion-and-rebuild.md).
2. Delete 90% and measure recall against a freshly-built index over the
   survivors. Where does the tombstone argument break down?
3. Change `search` to skip tombstones *during* traversal and re-measure recall at
   50% deleted. This is the ADR's central claim — does it hold?
4. Instrument the search to count nodes visited versus results returned, at 0%
   and 90% deleted. That ratio is what tombstones cost per query.
5. Sketch a reverse-edge index that would make physical deletion practical. Cost
   it in bytes, and decide whether it beats periodic compaction.

## Where this lives in the code

- `src/storage/vector_store.cpp` — `remove`
- `src/index/hnsw/hnsw_index.cpp` — `remove`, and the filter in `search`
- `src/db/database.cpp` — `compact`, and the `check` warning
- `tests/unit/index_hnsw_test.cpp` — `RecallSurvivesHeavyDeletionBecauseTombstonesStillRoute`

---

Next: [../90-debugging/01-how-to-debug-this-project.md](../90-debugging/01-how-to-debug-this-project.md)
