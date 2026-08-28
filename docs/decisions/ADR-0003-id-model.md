# ADR-0003 — Two id types: stable `VectorId`, dense `LocalId`

**Status:** Accepted

## Decision

Two distinct identifiers, never interchangeable:

| | Type | Range | Stable? | Used by |
|---|---|---|---|---|
| `VectorId` | `uint64_t` | arbitrary | yes, forever | users, metadata, search results |
| `LocalId` | `uint32_t` | `[0, count)` | no — compaction renumbers | storage offsets, HNSW edges |

A bidirectional mapping is maintained between them.

## Context

The index needs to name vectors constantly: every HNSW node stores up to `M`
neighbour references per layer, and the search loop dereferences them one after
another. The user also needs to name vectors, with something that survives
restarts, deletions and rebuilds.

These are not the same requirement, and satisfying both with one identifier
means satisfying neither well.

## Options considered

1. **One 64-bit id everywhere.** Simple. No mapping table.
2. **One 32-bit id everywhere.** Compact, but caps user ids at 4 billion and
   makes id reuse after deletion observable to the user.
3. **Two ids with a mapping.** (chosen)

## Chosen approach

Option 3. `VectorId` is what crosses the API boundary. `LocalId` is an index
into `VectorArray` and never escapes the engine.

## Why

**Memory.** HNSW's edge data dominates its footprint. For N = 1,000,000, M = 16:

```
edges ≈ N × M × 2          (layer 0 gets 2M neighbours)
      ≈ 32,000,000 references

32-bit references:  128 MB
64-bit references:  256 MB
```

128 MB saved, on a machine where the vectors themselves are 3 GB and the
working set decides whether the graph traversal hits L2 or DRAM. Halving the
edge data roughly halves the cache pressure of the pointer-chasing phase, which
is the part of HNSW search that is *not* arithmetic-bound.

**Locality.** A dense `LocalId` makes vector lookup `data() + id * dimension` —
one multiply-add. With a sparse 64-bit id it would be a hash probe per
candidate, in the inner loop, on data that does not fit in cache.

**Compaction.** Tombstoned slots must eventually be reclaimed. A dense internal
id makes compaction a renumbering; the user's `VectorId`s are unaffected, which
is precisely why they must be a separate space.

## Trade-offs

| Cost | Benefit |
|---|---|
| Two lookup structures to keep in sync | 2× smaller graph, cache-friendly scans |
| A bug class: passing the wrong id type | Compaction without breaking user references |
| Hard ceiling of 2^32 − 2 vectors per database | Direct indexing with no hashing |

The "wrong id type" bug is real and would be a silent one — both are integers.
Mitigation: distinct type aliases, `LocalId` never appearing in a public API,
and naming discipline (`local_id` vs `id`). A stronger mitigation would be
opaque strong typedefs; see below.

The 2^32 ceiling is a deliberate limit, asserted by a unit test, and documented
in the README rather than discovered by a user at 4.3 billion vectors.

## Consequences

- Every index implementation stores `LocalId`, not `VectorId`.
- Search results are translated back to `VectorId` exactly once, at the
  database boundary, after the top-k is final — translating earlier would put a
  lookup in the inner loop.
- The mapping is part of the persisted state and must be validated on load: a
  `LocalId` in a neighbour list that exceeds the node count is corruption.

## Alternatives for future versions

Strong typedefs (`struct LocalId { uint32_t value; }`) would make mixing the
two a compile error rather than a silent bug. Rejected for v1 only because the
arithmetic-heavy call sites become noisier, and the naming discipline has held
so far. If a real mix-up bug ever occurs, this is the fix.
