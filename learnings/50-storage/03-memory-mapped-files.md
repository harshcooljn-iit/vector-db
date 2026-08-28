# Memory-mapped files — making a file look like an array

🔴 Advanced · pairs with `MappedFile` in `src/persistence/file_io.cpp`

## Normal file I/O

```cpp
std::vector<std::byte> buffer(size);
read(fd, buffer.data(), size);
```

What actually happens:

```
disk ──DMA──> kernel page cache ──memcpy──> your buffer
                (kernel memory)             (your memory)
```

**The data exists twice.** To read a 3 GB file you need 3 GB of your own memory,
plus whatever the kernel keeps cached, and you paid a 3 GB `memcpy` to get it.

## Memory mapping

```cpp
void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
const float* floats = static_cast<const float*>(base);
floats[1000000];        // just an array access
```

You get back an address. Reading through it reads the file. There is no copy and
no `read` call.

To understand why that is possible, you need one idea from operating systems.

## Virtual memory, in one diagram

Your program's pointers are **virtual addresses**. They are not memory
locations; they are entries in a per-process table the CPU consults on every
access.

```
your pointer 0x7f8a4000
        |
        v
   [ page table ]  ──> physical RAM frame 0x1a2b000
        |
        └──────────> or: "not resident — go get it"
```

The unit is a **page**, typically 4 KB (16 KB on Apple Silicon).

`mmap` adds entries to that table saying *"this range of addresses corresponds
to this range of that file"* — and marks them not-yet-resident. It does not read
anything. Mapping a 3 GB file takes microseconds.

## Page faults

When you touch an address whose page is not resident:

1. The CPU traps into the kernel — a **page fault**.
2. The kernel sees the address is backed by a file, and reads that page (plus
   some read-ahead).
3. It updates the page table.
4. Your instruction is **restarted**, and now succeeds.

Your code sees none of this. It looks like a memory access that was
occasionally slow.

The consequences are what matter:

- **Only what you touch is read.** Search the first 1000 vectors of a 3 GB file
  and you read a few hundred KB.
- **The OS manages the cache.** Under memory pressure it evicts clean pages
  without asking. A read-only mapping can never fail for lack of memory — it
  just gets slower.
- **Multiple processes share one copy.** Two `vectordb` processes mapping the
  same file share the same physical pages.
- **You can map more than you have RAM.** 8 GB of RAM will happily map a 30 GB
  file. Whether it *performs* depends entirely on your access pattern.

## Why this project can use it at all

This is the part that ties back to earlier decisions.

`mmap` gives you the file's bytes. To use them as floats without a conversion
pass, the file layout must **already be** the memory layout:

```cpp
const auto* floats = reinterpret_cast<const float*>(bytes.data() + payload_offset);
VectorView v{floats + slot * dimension, dimension};   // no copy, ever
```

Three earlier decisions make that legal:

1. **Contiguous storage with stride = dimension**
   ([ADR-0004](../../docs/decisions/ADR-0004-contiguous-storage.md)). Padding
   the rows would have forked the memory layout from the file layout and
   forfeited exactly this.
2. **A 64-byte header**, so the payload starts page- and cache-line-aligned.
   Casting to `const float*` at a misaligned address is undefined behaviour and
   slow where it works at all.
3. **Little-endian, IEEE-754 float32**, enforced by a `static_assert`, so the
   bytes on disk are bit-for-bit the bytes in registers.

Every one of those looked like fussiness at the time. This is what they were for.

## Trade-offs

| Advantage | Cost |
|---|---|
| No copy; the file *is* your array | An I/O error becomes **SIGBUS**, not an exception |
| Only touched pages are read | Page faults are invisible in a profile — the time shows as "slow memory" |
| OS handles caching and eviction | Random access thrashes if the working set exceeds RAM |
| Shared between processes | A file truncated *while mapped* means SIGBUS on the vanished pages |
| Mapping is O(1) regardless of size | 32-bit address space cannot map large files (irrelevant here) |

The SIGBUS point is the genuinely uncomfortable one. With `read()`, a disk error
returns `EIO` and you throw. With `mmap`, the failing access is an instruction
that cannot complete, and the kernel sends SIGBUS — which by default kills the
process, and cannot be turned into a C++ exception portably. This is a real
robustness cost, accepted here because the alternative is not being able to open
a database larger than RAM.

## `madvise` — telling the kernel what you are about to do

```cpp
mapping.advise_sequential();   // MADV_SEQUENTIAL
mapping.advise_random();       // MADV_RANDOM
```

Pure hints; ignoring them is always correct. But they matter here, and the two
call sites want opposite things:

- **Loading a vector file** is one front-to-back pass. `MADV_SEQUENTIAL` tells
  the kernel to read far ahead and to drop pages behind you, so a 3 GB load does
  not evict everything else in the machine.
- **Traversing an HNSW graph** jumps to unpredictable nodes. `MADV_RANDOM`
  disables read-ahead, which would otherwise fetch 16 pages to use 1 — wasting
  bandwidth on a workload that is already latency-bound.

Same file, same API, opposite advice, because the access pattern differs. Which
is itself the lesson: **the kernel's default is tuned for the average program,
and you know more about your access pattern than it does.**

## The state of it in this project

Implemented and used: `read_vector_file()` maps the file and constructs the
store from the mapped floats.

**Not yet implemented**: keeping the mapping alive and searching *directly* over
it, with no in-memory `VectorStore` at all. That is the version that would let a
30 GB database open on an 8 GB machine, and it needs the store to be able to
hold a borrowed, read-only payload instead of an owned `VectorArray`.

Stated plainly rather than implied: today's load still materialises the whole
store in memory. The mapping saves the second copy and the explicit `read`, not
the resident footprint.

## Experiments

1. Write a 1 GB file. Time `read()`-ing it into a buffer versus `mmap`-ing it
   and touching only the first page. Then `mmap` it and touch every page.
2. Use `/usr/bin/time -l` (macOS) or `-v` (Linux) to compare peak RSS for
   `read` versus `mmap` on a large vector file.
3. Map a file, then truncate it from another shell, then touch the vanished
   pages. Observe SIGBUS. This is the failure mode you are accepting.
4. Map a large file and touch pages in a random permutation versus in order.
   Compare. You have measured what `MADV_RANDOM` exists for.
5. Open the same database from two processes at once and compare total system
   memory against one process. The pages are shared.

## Where this lives in the code

- `include/vectordb/persistence/file_io.hpp` — `MappedFile`, and why `mmap` is
  possible here at all
- `src/persistence/file_io.cpp` — `mmap`/`munmap`/`madvise`, including the
  zero-length special case (`mmap` of an empty file fails with `EINVAL`)
- `src/storage/vector_file.cpp` — `read_vector_file`, which maps and advises
- `docs/vector-storage.md` — the layout the mapping depends on

---

Next: [04-sqlite.md](04-sqlite.md)
