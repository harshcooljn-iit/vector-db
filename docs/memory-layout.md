# Memory layout

Where the bytes go, and why. The decisions here shape everything above them.

Learning note: [10-cpp-systems-foundations/03](../learnings/10-cpp-systems-foundations/03-memory-layout.md).

## The principle

> **Lay data out the way the hot loop reads it.**

Every decision below is that principle applied to a different loop.

## Vectors: one contiguous block

```
vectors_:  [ v0: D floats ][ v1: D floats ][ v2: D floats ] ...
           |<----------- one allocation ----------->|
```

Vector `i` is at `data() + i * dimension`. One multiply, no indirection.

The alternative — `std::vector<std::vector<float>>` — costs, at N = 1M, D = 768:
a million allocations, ~40 MB of pointer and header overhead, and (the decisive
one) inner buffers scattered in *allocation* order rather than *scan* order. The
prefetcher is excellent at "keep reading forward" and useless at "guess where
this pointer goes", so the hottest loop in the system becomes DRAM-latency-bound
instead of bandwidth-bound.

[ADR-0004](decisions/ADR-0004-contiguous-storage.md).

### Stride equals dimension

No padding to a SIMD or cache boundary. Padding would let kernels drop tail
handling — zero-filled padding contributes `(0−0)² = 0` — but it would fork the
in-memory layout from the on-disk layout and forfeit direct memory mapping.

`VectorArray::stride()` exists separately from `dimension()` so that reversing
this decision would be contained. It is a measurement away, not a rewrite.

## Metadata about vectors: parallel arrays

```
vectors_       [ v0 ][ v1 ][ v2 ]      D floats each
norms_         [ n0 ][ n1 ][ n2 ]      4 bytes each
live_          [  1 ][  0 ][  1 ]      1 byte each
local_to_id_   [ 42 ][  7 ][ 99 ]      8 bytes each
```

Struct-of-arrays, not array-of-structs, because these are read by different loops
at wildly different rates:

| Array | Read | Per query |
|---|---|---|
| `vectors_` | distance loop | N times |
| `norms_` | distance loop | N times |
| `live_` | scan filter | N times |
| `local_to_id_` | result translation | **k times** |

Interleaving would drag the 8-byte id — and its padding — through every cache
line the distance loop pulls in, to serve a k-times-per-query need.

### Liveness is a byte, not a bit

1 MB per million vectors against 3 GB of payload: 0.03%. A packed bitset would be
125 KB and would need a shift and a mask in the inner loop. Not worth it —
though it would be if this ever became the working set, which it is not.

## HNSW layer 0: flat, fixed stride

```
layer0_links_:  [cnt][n0][n1]...[n31] [cnt][n0]...
                |<--- 2M + 1 = 33 --->|
```

Every node is here and this layer takes essentially all the search traffic, so
it gets the simplest possible layout: one multiply to find a node's block.

**The count is in slot 0**, not a parallel array. On a 64-byte cache line that is
16 `LocalId`s, so reading "how many neighbours" pulls in the first 15 of them for
free — and you were about to read them anyway. A separate count array would cost
a second cache miss on every node visit, in the innermost loop of the system.

At N = 1M, M = 16 this array is `1M × 33 × 4` = **132 MB** with a 32-bit
`LocalId`, and 264 MB with a 64-bit one. That is what
[ADR-0003](decisions/ADR-0003-id-model.md) bought.

## HNSW upper layers: a shared arena

Only ~1/M of nodes reach layer 1, ~1/M² reach layer 2. A fixed stride across all
nodes would be ~94% empty.

```
upper_offsets_: [kNone][ 0 ][kNone][ 34 ]...      one u32 per node
upper_links_:   [cnt][n0]..[n15][cnt][n0]..       append-only arena
```

Append-only, because a node's level is fixed at insertion and never changes.
Nothing is freed or moved, so an offset stays valid forever.

**Hazard worth naming**: `upper_offsets_[node]` is `0xFFFFFFFF` for a level-0
node, and the arithmetic would land wildly out of bounds. It is safe only because
every caller goes through `neighbours()`, which checks `layer > node_level_[node]`
first. An invariant enforced by discipline, not by the type system.

## On disk: the same layout

```
vectors.bin:  [header 64B][ float payload ][ ids ][ norms ][ liveness ]
```

Separate regions, matching the in-memory arrays. Two reasons, the first decisive:

1. The payload region **is** `VectorArray`'s memory layout, byte for byte, so a
   load can `mmap` and use the floats in place — no copy, no transformation pass.
2. The distance loop reads floats and nothing else; an interleaved id would drag
   8 unread bytes into every cache line.

The 64-byte header exists so the payload starts page- and cache-line aligned.
Casting a misaligned address to `const float*` is undefined behaviour.

## Footprint

For N vectors, dimension D, HNSW with parameter M:

| Component | Bytes |
|---|---|
| Vectors | `N × D × 4` |
| Norms | `N × 4` |
| Liveness | `N` |
| Id map | `N × 8` + hash map |
| HNSW layer 0 | `N × (2M + 1) × 4` |
| HNSW upper | ~`N/(M−1) × (M+1) × 4 × 1.2` |
| HNSW offsets + levels | `N × 5` |

At N = 1,000,000, D = 768, M = 16:

```
vectors      3.07 GB     94.2%
id map      ~0.02 GB      0.6%
HNSW        ~0.15 GB      4.6%
other        0.005 GB     0.2%
────────────────────────────────
total       ~3.25 GB
```

**The vectors dominate.** That is why quantization is the highest-value future
work: halving the payload saves more than eliminating everything else combined.

Measured at N = 50,000, M = 16: 145 bytes/vector of index, independent of
dimension — 28% overhead at D = 128, 4.7% at D = 768.

## Verifying

```sh
vectordb stats mydb
```

Reports resident memory split by component, on-disk sizes, and index overhead per
vector.
