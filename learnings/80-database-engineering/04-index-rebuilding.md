# Rebuilding — the escape hatch that makes everything else safe

🟡 Intermediate

## The idea

An index is **derived data**. Anything derived can be thrown away and
reconstructed, and that single fact removes an enormous amount of difficulty
elsewhere.

```cpp
void Database::rebuild_index() {
    impl_->build_index();     // replay add() over every slot
    impl_->dirty_ = true;
    flush();
}
```

## What it buys

**Corruption stops being fatal.** A bad index file is logged and rebuilt. Opening
never fails because of it, and there are tests for truncated, garbage and missing
index files.

**Format changes stop being migrations.** Bump `kHnswFileVersion`, ship it, and
every old index rebuilds on first open. No migration code, no dual-read path.

**Parameter changes become possible.** `M` and `efConstruction` are baked into
the graph, but "rebuild with different parameters" is a supported operation
rather than a rewrite.

**Tombstones become collectable.** `compact` renumbers slots and rebuilds, which
is only safe because the graph can be reconstructed from scratch.

## The cost

Measured at N = 50,000, D = 128:

| | |
|---|---|
| Rebuild | **7.8 s** |
| Load a valid index | **9 ms** |

860×. Which is why the index is persisted at all — and why the rebuild is a
fallback rather than the normal path.

Rebuild is `O(N log N)` searches, so at 1M vectors it is minutes. Acceptable as
recovery; unacceptable per open.

## Rebuild must be identical

```cpp
for (LocalId id = 0; id < store_.slot_count(); ++id) {
    hnsw->add(id);
    if (!store_.live(id)) hnsw->remove(id);   // tombstoned slots still get added
}
```

Note the second line. A tombstoned slot is added and *then* marked dead, rather
than skipped.

Skipping would break `add`'s ascending-slot contract and renumber every node —
so the rebuilt graph would not correspond to the store's slots at all. A subtle
bug that would produce a working-looking index returning wrong ids.

Because level assignment is seeded and insertion order is the store's slot order,
a rebuild from the same data produces a **byte-identical** graph. There is a test.

## When it happens

| Trigger | Automatic? |
|---|---|
| Index file missing | yes, on open |
| Index fails validation | yes, on open, logged |
| Metric or dimension mismatch | yes, on open, logged |
| Node count disagrees with the store | yes, on open, logged |
| `vectordb rebuild-index` | manual |
| `vectordb compact` | as part of it |

Automatic recovery is **logged, never silent**. A database that quietly rebuilt
on every open would be a mystery rather than a feature — you would notice only
as unexplained slow starts.

## Compaction is a rebuild with renumbering

```cpp
std::size_t Database::compact() {
    // copy live slots into a fresh store — LocalIds renumbered, VectorIds kept
    // rebuild the graph over the new slots
}
```

This is the payoff of the two-id design
([ADR-0003](../../docs/decisions/ADR-0003-id-model.md)): internal slots are
renumbered freely and no user-visible reference changes. There is a test
asserting exactly that.

## The general principle

> **Design so that the artefact most likely to be corrupt is the one you can
> rebuild.**

Applied here: the index is derived from the vector store, so corruption there is
a slow open rather than data loss. The vector store is authoritative, which is
why it gets atomic replacement, a checksum and nine validation steps.

The same idea appears everywhere. Caches, secondary indexes, materialised views,
derived aggregates — anything you can recompute is something you do not have to
protect as hard. Being deliberate about which artefacts are which is a large part
of storage design.

## Experiments

1. Delete `index.hnsw` and reopen with `--verbose`. Then corrupt it and compare
   the log lines.
2. Time `rebuild-index` at N = 10k, 100k, 1M. Confirm the `O(N log N)` shape.
3. Rebuild twice and compare the graphs byte for byte. Then change the seed.
4. Remove the `hnsw->remove(id)` line for tombstoned slots and find out what
   breaks — the failure is *not* an exception.
5. Bump `kHnswFileVersion`, rebuild the binary, and open an old database. That is
   your migration path, and it took no migration code.

## Where this lives in the code

- `src/db/database.cpp` — `build_index`, `rebuild_index`, `compact`, `open`
- `tests/integration/database_test.cpp` — `RebuildsARuinedIndexOnOpenInsteadOfFailing`
- `docs/persistence.md`

---

Next: [05-tombstones.md](05-tombstones.md)
