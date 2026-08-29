# ADR-0011 — Delete with tombstones, reclaim on compaction

**Status:** Accepted

## Decision

Deleting a vector clears a liveness byte and drops its metadata. The floats stay
where they are, and the HNSW node stays in the graph, still routing traffic but
never returned. `compact` reclaims the space.

## Context

Deletion is the operation graph indexes handle worst, and it is worth being
precise about why.

Physically removing node `n` means:

1. finding every node that lists `n` as a neighbour — the graph stores no
   reverse index, so that is a full scan;
2. repairing each of those lists, which may need a fresh neighbour-selection
   pass;
3. renumbering, if slots are compacted, which invalidates *every* stored edge;
4. and accepting that `n` may have been the only bridge between two regions.

Point 4 is the dangerous one. Cutting a bridging node can disconnect part of the
graph — silently. Nothing errors; recall simply collapses for whatever ended up
on the wrong side, and it does so for queries you have not run yet.

## Options considered

1. **Physical removal with graph repair.** Correct in principle, expensive, and
   with a disconnection failure mode that is invisible until measured.
2. **Rebuild the whole index on every delete.** `O(N log N)` per deletion.
3. **Tombstones, with periodic compaction.** (chosen)

## Chosen approach

Option 3.

- `VectorStore` clears `live_[slot]` and erases the id from its lookup map.
- `HnswIndex` decrements a counter; the node and all its edges remain.
- `search` filters tombstoned nodes out of the results **after** the traversal.
- `compact` rewrites the store without dead slots and rebuilds the graph.

## Why

- **Deletion becomes `O(1)`** instead of a graph-wide repair.
- **The graph stays connected.** A tombstoned node keeps bridging exactly what
  it bridged before. There is a test that deletes half the vectors from a
  3,000-vector index and confirms recall stays above 0.85 — the evidence that a
  dead node still earns its keep as a router.
- **Filtering after traversal, not during**, is what preserves that. Skipping
  tombstones while walking would cut their edges and reintroduce the
  disconnection problem.
- It is what almost every storage engine does, for the same reasons.

## Trade-offs

| Cost | Benefit |
|---|---|
| Deleted vectors still occupy memory and disk | Deletion is O(1) |
| The traversal still visits them | The graph cannot become disconnected |
| Search must over-fetch slightly to fill k | No neighbour lists to repair |
| Compaction is a separate, explicit step | Deletion cannot corrupt the index |

The over-fetch is small in practice and bounded by the tombstone ratio, which
`vectordb stats` reports and `vectordb check` warns about above 50%.

## Consequences

- `slot_count` and `live_count` are different numbers, and both are persisted.
  Every layer that walks slots has to know the difference.
- `LocalId` and `VectorId` must be separate id spaces, or compaction's
  renumbering would break every reference a user holds. That is
  [ADR-0003](ADR-0003-id-model.md), and this is what it was for.
- `search` filters after the traversal, which is why `k` results can require
  visiting more than `k` nodes.
- The vector file stores the liveness array, so tombstones survive a restart. A
  reload that resurrected deleted vectors would be a data-integrity bug; there
  is a test named for it.

## Alternatives for future versions

- **Automatic compaction** when the tombstone ratio crosses a threshold. Today
  it is manual, so it never surprises anyone with a long pause.
- **Incremental repair**: when a node is tombstoned, connect its neighbours to
  each other so it can eventually be cut without disconnecting anything. This is
  roughly what FreshDiskANN does.
- **A reverse edge index**, making "who points at me?" cheap and physical
  deletion practical. Costs another `N × M` references.
