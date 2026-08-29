# Cache locality — the 100× you cannot see

🟡 Intermediate → the consequences are advanced

## The hierarchy

Your mental model is probably "the CPU reads memory". The reality, on an Apple M1:

| Level | Size | Latency | Relative |
|---|---|---|---|
| Register | ~KB | 0 cycles | 1× |
| L1 data | 128 KB/core | ~4 cycles | 4× |
| L2 | 4 MB shared | ~15 cycles | 15× |
| DRAM | 8 GB | ~100–300 cycles | **~100×** |

A DRAM access costs about as much as a hundred arithmetic operations. **Where
your data is matters more than what you do to it.**

## Cache lines

Memory moves in **cache lines** — 64 bytes on x86, 128 on Apple Silicon. Read one
float and the hardware fetches the whole line.

```
read v[0]  → fetches v[0..31]   (128-byte line, 32 floats)
read v[1]  → free
read v[32] → another line
```

Sequential access gets 32 floats per DRAM trip. Random access gets 1.

That is a **32× difference in effective bandwidth**, from access pattern alone.

## The prefetcher

The CPU watches your access pattern. Walk forward through memory and it fetches
lines *ahead of you*, so by the time you ask, the data is in L1.

It cannot follow pointers. It does not know that the value at `vectors[i]` is an
address you are about to chase.

> Sequential access is **bandwidth-bound** (fast). Pointer chasing is
> **latency-bound** (slow), and no amount of arithmetic optimisation helps.

## Applied: the vector layout

This is the whole argument for
[ADR-0004](../../docs/decisions/ADR-0004-contiguous-storage.md).

```
std::vector<std::vector<float>>      one million scattered allocations
std::vector<float>                   one block, in scan order
```

The scattered version defeats the prefetcher. Every candidate is a potential
DRAM stall, and the scan becomes latency-bound.

Contiguous, the scan is a straight walk. Measured: brute force sustains **38–48
GB/s**, which is this machine's memory bandwidth. You cannot do better than the
hardware.

## Applied: struct of arrays

```
vectors_       [ v0 ][ v1 ][ v2 ]     read N times per query
norms_         [ n0 ][ n1 ][ n2 ]     read N times
live_          [  1 ][  0 ][  1 ]     read N times
local_to_id_   [ 42 ][  7 ][ 99 ]     read k times
```

Interleaving these into one struct would pull the 8-byte id — and its padding —
into every cache line the distance loop touches, to serve something needed k
times rather than N.

**Group data by how often it is read, not by what it describes.**

## Applied: the neighbour count in slot 0

```
layer0_links_:  [cnt][n0][n1]...[n31]
```

The count shares a cache line with the first 15 neighbours it describes — and you
were about to read them anyway. A parallel count array would cost a second cache
miss on **every node visit**, in the innermost loop of the system.

## Applied: LocalId is 32-bit

At N = 1M, M = 16, the layer-0 link array is 132 MB at 32 bits and 264 MB at 64.

That is not just memory. HNSW's traversal is the pointer-chasing case — it jumps
to unpredictable nodes — so it is latency-bound, and halving the data halves how
much of it fits in cache. [ADR-0003](../../docs/decisions/ADR-0003-id-model.md).

## The two workloads, side by side

| | Brute force | HNSW traversal |
|---|---|---|
| Access pattern | perfectly sequential | unpredictable jumps |
| Prefetcher | works perfectly | useless |
| Bound by | bandwidth | latency |
| SIMD helps | barely | more |
| `madvise` hint | `SEQUENTIAL` | `RANDOM` |

Same data, same file, opposite advice. Which is the lesson: **the kernel's
default is tuned for the average program, and you know more about your access
pattern than it does.**

## False sharing

Two threads writing *different* variables that share a cache line. The line
ping-pongs between cores as if they were sharing data, and throughput collapses.

```cpp
struct Counters { std::atomic<int> a; std::atomic<int> b; };   // same line
```

Fix with padding to `std::hardware_destructive_interference_size`.

VectorDB avoids it structurally: `batch_search` writes `results[i]`, and each
element is a `std::vector` — 24 bytes, so adjacent ones can share a line, but
they are written once each rather than hammered. **Reads never cause false
sharing**; only writes do.

## Measuring

```sh
perf stat -e cache-misses,cache-references,cycles,instructions ./vectordb-bench
sysctl hw.l1dcachesize hw.l2cachesize hw.cachelinesize     # macOS
lscpu | grep -i cache                                      # Linux
```

An IPC below ~1 usually means you are stalling on memory.

## Experiments

1. Scan 100,000 vectors in order, then in a random permutation. Same data, same
   arithmetic. The ratio is the prefetcher.
2. Time a scan at N = 100 … 1,000,000 and plot ns per vector against working-set
   size. The steps are your cache levels.
3. Interleave the id into `VectorArray` and measure the scan.
4. Move the neighbour count out of slot 0 into a parallel array and measure HNSW
   search.
5. Two threads incrementing adjacent `atomic<int>`s, then the same padded to 128
   bytes. That gap is false sharing.

## Where this lives in the code

- `include/vectordb/core/vector_array.hpp` — the layout and its reasoning
- `include/vectordb/index/hnsw_index.hpp` — the two link layouts
- `src/persistence/file_io.cpp` — `advise_sequential` / `advise_random`
- `docs/memory-layout.md`, `docs/benchmark-results.md` §2

---

Next: [02-allocations.md](02-allocations.md)
