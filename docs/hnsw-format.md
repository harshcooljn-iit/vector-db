# HNSW index format — `index.hnsw`

The persisted graph. **Derived data**: if this file is missing, corrupt, or
inconsistent with the vector store, it is rebuilt rather than trusted.

Implementation: `src/index/hnsw/hnsw_file.cpp`. Structure: [hnsw.md](hnsw.md).

## Conventions

Same as [`vectors.bin`](vector-storage.md): little-endian, explicit field
serialization for the header, and a fixed 128-byte header so the arrays that
follow start cache-line aligned.

## Layout

```
offset  size                     field
------  -----------------------  --------------------------------------------
     0  8                        magic "VDBHNSW\0"
     8  4                        format_version (u32) = 1
    12  4                        dimension (u32)          cross-check
    16  8                        node_count (u64)         incl. tombstones
    24  8                        live_count (u64)
    32  4                        M (u32)
    36  4                        ef_construction (u32)
    40  4                        ef_search (u32)
    44  4                        entry_point (u32)
    48  8                        seed (u64)
    56  8                        rng_state (u64)
    64  4                        max_level (u32)
    68  1                        metric (u8)              cross-check
    69  3                        reserved, zero
    72  8                        layer0_link_count (u64)
    80  8                        upper_link_count (u64)
    88  4                        payload_crc32 (u32)
    92  32                       reserved, zero
   124  4                        header_crc32 (u32) over [0, 124)
------  -----------------------  --------------------------------------------
   128  node_count               node_level: one u8 per node
     L  node_count*4             upper_offsets: u32 per node (0xFFFFFFFF = none)
     O  layer0_link_count*4      layer0_links: flat, stride 2M+1
     Z  upper_link_count*4       upper_links: arena, stride M+1 per (node, layer)
```

The four arrays are written as raw little-endian bytes — the same narrow
exception as the vector payload, and safe for the same reason: a `static_assert`
fails the build on a big-endian host, and these are fixed-width types.
Serializing 30 million `LocalId`s one shift at a time would be absurd.

## Link block layout

Both link arrays use the same shape: **the count occupies slot 0**, followed by
that many neighbour ids.

```
layer 0, node n:   layer0_links[n * (2M+1)]      = count
                   layer0_links[n * (2M+1) + 1..] = neighbours

layer L, node n:   base = upper_offsets[n] + (L-1) * (M+1)
                   upper_links[base]      = count
                   upper_links[base + 1..] = neighbours
```

Putting the count in slot 0 rather than a parallel array means reading "how
many" pulls the first neighbours into the same cache line — and you were about to
read them anyway.

## Validation

The header goes through the same ladder as the vector file: size, magic, version,
header CRC, field ranges, overflow-checked arithmetic, and an exact file-size
comparison distinguishing truncation from trailing data.

`layer0_link_count` must equal `node_count × (2M + 1)` exactly — layer 0 has a
fixed stride, so its size is fully determined, and a mismatch means the header is
internally inconsistent.

The stored configuration is run through `HnswConfig::validate()`, so a corrupt
`M` cannot produce a graph the constructor would have refused to build.

### Reference validation — the expensive part

**Every neighbour reference is checked before the graph is used.** `O(edges)`,
and it buys two distinct things:

1. **The search loop never bounds-checks a neighbour id.** A corrupt file cannot
   become an out-of-bounds read at query time.
2. **A damaged graph is reported as damaged** rather than returning plausible
   wrong answers forever. A graph with one bad edge does not crash — it quietly
   loses recall.

Per node:

- level ≤ `max_level`
- neighbour count ≤ the layer's budget (`2M` on layer 0, `M` above)
- every reference in `[0, node_count)`
- no self-link
- **no link on layer L to a node that only reaches layer L−1** — that reference
  would point into unallocated arena
- `upper_offsets[n] + level × (M+1) ≤ upper_link_count`, checked **before** the
  offset is used as an index
- a level-0 node must have `upper_offsets[n] == 0xFFFFFFFF`
- the entry point's level equals `max_level`

## Cross-artefact checks

Three things make an index not merely stale but *meaningless*, and each is
rejected with a message saying to rebuild:

| Check | Why it matters |
|---|---|
| `metric` matches the database | edges encode proximity under rules that no longer apply |
| `dimension` matches | the graph describes vectors of a different shape |
| `node_count` equals the store's slot count | slots and nodes are not in correspondence |

## What is not verified on open

The payload CRC, matching the vector file. A flipped bit inside a neighbour list
may leave the reference in range, so structural validation can miss it —
`vectordb check --deep` catches it, and there are tests for both halves.

## Determinism

`seed` and `rng_state` are persisted, so a loaded index continues generating
levels exactly where the original left off. Two indexes built from the same data,
config and seed have byte-identical graphs, and a test walks every neighbour list
of both to prove it.

## Size

```
bytes ≈ 128 + node_count × (5 + (2M + 1) × 4) + upper arena
```

At M = 16, N = 50,000: **6 MB** measured, ~145 bytes/vector including the arena.
Independent of dimension, because edges do not scale with D.

## Version history

| Version | Change |
|---|---|
| 1 | Initial format. |
