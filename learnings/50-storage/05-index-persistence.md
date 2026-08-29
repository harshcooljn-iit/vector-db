# Persisting a graph

🔴 Advanced · pairs with `src/index/hnsw/hnsw_file.cpp`

## Why not just rebuild?

Measured at N = 50,000, D = 128:

| | |
|---|---|
| Rebuild the index | **7.8 s** |
| Load and validate it | **9 ms** |

**860×.** That is the entire argument. At a million vectors it would be minutes
against a fraction of a second.

## What makes a graph harder than an array

A vector array is values. A graph is values **plus references**, and references
are what can be wrong in interesting ways.

```
node 5, layer 0: [3][ 7 ][ 12 ][ 4294967295 ]
                            ^^^^^^^^^^^^^^^^ ← there is no node 4294967295
```

Nothing about that is structurally detectable from sizes alone. It only becomes a
problem when the search loop dereferences it — as an out-of-bounds read, at query
time, in a hot loop.

## The design

```
[header 128B]
[node_level:    one u8 per node        ]
[upper_offsets: one u32 per node       ]
[layer0_links:  flat, stride 2M+1      ]
[upper_links:   arena, stride M+1      ]
```

Flat arrays, not a serialized object graph. Each is written as raw
little-endian bytes — the same narrow, `static_assert`-enforced exception as the
vector payload, because serializing 30 million ids one shift at a time would be
absurd.

**Nothing is a pointer.** Everything is an index, which is what makes the format
position-independent and validatable.

## Validate every reference, before use

`O(edges)`. Deliberately.

```cpp
if (neighbour >= header.node_count)          throw CorruptionError(...);
if (neighbour == node)                       throw CorruptionError(...);
if (levels[neighbour] < layer)               throw CorruptionError(...);
if (count > budget)                          throw CorruptionError(...);
if (offset + level * stride > arena_size)    throw CorruptionError(...);
```

It buys two things, and they are worth the pass:

1. **The search loop never bounds-checks.** A corrupt file cannot become an
   out-of-bounds read at query time. The validation pays for itself in
   performance.
2. **A damaged graph is reported.** Silently searching a corrupt graph returns
   plausible wrong answers forever — the worst possible failure mode.

That third check is subtle and worth staring at: a link on layer L to a node
whose level is L−1 would index into arena space that was never allocated for it.
Structurally the offset is in range; semantically it is garbage.

## Cross-artefact checks

Three things make an index not merely stale but **meaningless**:

| Mismatch | Why it is fatal |
|---|---|
| metric | edges encode proximity under rules that no longer apply |
| dimension | the graph describes vectors of a different shape |
| node count vs store slots | nodes and slots are not in correspondence |

Each error message ends with "rebuild the index", because the vector store is
authoritative and rebuilding is always available.

## Determinism across a save

`seed` and `rng_state` are both persisted, so a loaded index continues generating
levels exactly where the original stopped.

Without this, inserting into a reloaded index would follow a different level
sequence than inserting into the original — and two databases with identical
histories would diverge. There is a test that walks every neighbour list of two
independently-built graphs and demands they match.

## When to persist

VectorDB writes the index on `flush`, after the vector file.

The ordering is deliberate: a crash between them leaves an index whose node count
disagrees with the store, which `open` detects and repairs. The reverse order
would leave an index describing vectors that were never written.

> **When you cannot make two writes atomic, order them so the detectable
> inconsistency is the recoverable one.**

## Incremental persistence — considered, rejected

Rewriting the whole graph on every flush is `O(N·M)`. An append-only log of
edge changes would be `O(changes)`.

Not done, because: HNSW insertion *rewrites* existing neighbour lists rather than
only appending, so the log would need to record replacements; replay would need
to be exactly as correct as the original construction; and at 6 MB for 50,000
vectors, a full rewrite is 113 ms. The complexity does not pay yet.

This is the sort of thing to revisit with a measurement, not a hunch.

## Experiments

1. Corrupt a neighbour id to a value past the node count. Confirm it is caught at
   load, then remove the check and see what happens at query time under `asan`.
2. Time `write_hnsw_file` and `read_hnsw_file` at N = 10k, 100k. Which is slower,
   and why? (Validation is a full pass; writing is one `write` per array.)
3. Delete the `rng_state` persistence and build two databases with identical
   histories, one via a save/load cycle. Compare the graphs.
4. Time the reference-validation pass separately from the rest of the load.
   Decide whether it is worth it at your N.

## Where this lives in the code

- `include/vectordb/index/hnsw_file.hpp` — the format constants
- `src/index/hnsw/hnsw_file.cpp` — write, header validation, reference validation
- `tests/unit/index_hnsw_file_test.cpp` — round trip plus one test per corruption
- `docs/hnsw-format.md`

---

Next: [../60-concurrency/01-threads.md](../60-concurrency/01-threads.md)
