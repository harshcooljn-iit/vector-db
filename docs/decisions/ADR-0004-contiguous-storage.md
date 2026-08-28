# ADR-0004 — Contiguous vector storage, stride equal to dimension

**Status:** Accepted

## Decision

All vectors in a database live in one `std::vector<float>` of
`count × dimension` elements. Vector `i` occupies
`[i × dimension, (i+1) × dimension)`. There is **no padding** between vectors.

## Context

The single hottest loop in the system is a linear scan computing a distance per
candidate. Brute force does it over every vector; HNSW does it over every
neighbour it visits. Whatever this loop touches, it touches millions of times.

## Options considered

### 1. `std::vector<std::vector<float>>`

The obvious design. At N = 1M, D = 768:

- **1,000,000 heap allocations** on ingest, and a million frees on shutdown.
- **~40 MB of overhead** — 24 bytes per inner vector plus a malloc header —
  before any payload.
- **Scattered layout.** Inner buffers land wherever the allocator put them,
  which is allocation order, not scan order. The hardware prefetcher is
  excellent at "keep reading forward" and useless at "follow this pointer", so
  it stops helping. The scan becomes DRAM-latency-bound instead of
  bandwidth-bound.

The third point is the decisive one and it is a large constant factor, not a
rounding error.

### 2. Contiguous block, stride = dimension (chosen)

One allocation. `data() + i * dimension`. The scan is a straight forward walk;
the prefetcher stays several cache lines ahead throughout.

### 3. Contiguous block, stride padded to a SIMD or cache boundary

Rounds each row up to a multiple of (say) 16 floats. Two genuine benefits:

- every row starts at the same alignment, so SIMD loads can use the aligned
  form;
- if the padding is zero-filled, `L2` and dot-product kernels can process
  `ceil(D/W)*W` elements with **no tail handling at all** — the padding
  contributes `(0-0)² = 0` and `0×0 = 0`.

That tail-elimination is a real and elegant optimization.

## Chosen approach

Option 2 — contiguous, unpadded — with the kernels handling their own tails.

## Why

- **The in-memory layout is byte-identical to the on-disk layout.** That is
  what will let `vectors.bin` be memory-mapped and used *directly*, with no
  transformation pass and no second copy. Padding would fork the two layouts:
  either the file wastes space too, or every load and store crosses a
  conversion. Given that memory mapping is how this scales past RAM, keeping
  the layouts identical is worth more than the tail branch.
- **The waste is not negligible for small dimensions.** Padding D=100 to 112 is
  12% more memory and 12% more bandwidth on the hottest loop. Bandwidth is the
  binding constraint; spending it to remove a branch that predicts perfectly is
  a bad trade.
- **On the dimensions that matter, alignment is already free.** 128, 384, 768
  and 1536 are all multiples of 4 (and of 16 floats for 128/384/768/1536 —
  check: 384/16 = 24 ✓, 768/16 = 48 ✓, 1536/16 = 96 ✓, 128/16 = 8 ✓). With
  stride = dimension, consecutive rows in these databases are already 64-byte
  aligned relative to the base. Padding would buy nothing for the common case
  and cost memory in the uncommon one.
- **Unaligned SIMD loads are not slow on the hardware we target.** ARM NEON
  `vld1q_f32` and x86 `_mm256_loadu_ps` run at full speed on modern cores when
  the access does not straddle a cache line, and mostly even when it does. The
  aligned-load argument is largely a relic of older microarchitectures.

This is a decision made from reasoning, and it is therefore provisional. It
will be re-examined with a measurement once the SIMD kernels exist and the tail
cost can actually be observed. If padding wins there, this ADR gets superseded
and the file format grows a `stride` field — which is why `VectorArray` already
exposes `stride()` separately from `dimension()` even though they are equal.

## Trade-offs

| Cost | Benefit |
|---|---|
| Every SIMD kernel needs tail handling | Memory layout == file layout, so mmap is direct |
| Rows have varying alignment for odd dimensions | No wasted memory or bandwidth |
| `push_back` can reallocate and copy everything | One allocation, prefetcher-friendly scans |

The reallocation cost is mitigated by `reserve()`, which bulk loads use; a unit
test pins that 1000 `push_back`s after `reserve(1000)` perform zero
reallocations.

## Consequences

- `VectorView`s are invalidated by anything that can reallocate — the same
  contract as `std::vector`, and the same discipline: never hold a view across
  an insertion.
- The database is capped by `LocalId` at 2^32 − 2 vectors, not by this class.
- `VectorArray::data()` can be handed straight to a file writer; the whole
  array goes out in one `write()`.

## Alternatives for future versions

- **Padded stride**, if measurement shows tail handling matters. The `stride()`
  accessor exists to make this a contained change.
- **Chunked storage** — a list of fixed-size blocks rather than one growing
  allocation. Avoids the copy on reallocation and avoids needing one enormous
  contiguous region, at the cost of a bounds check per access. This is what a
  system holding 100M vectors would do.
- **Quantized rows** (int8 / product quantization), which changes the element
  type but not the contiguity argument.
