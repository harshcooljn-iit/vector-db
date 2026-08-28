# Vector storage format — `vectors.bin`

The authoritative copy of a database's vectors. Everything else — indexes,
caches, results — is derived from this file and can be rebuilt from it.

Implementation: `src/storage/vector_file.cpp`.
Learning note: [`learnings/50-storage/02-binary-file-formats.md`](../learnings/50-storage/02-binary-file-formats.md).

## Conventions

- **Endianness**: little-endian throughout. Every supported target is
  little-endian, and rather than ship a byte-swapping path we cannot test, the
  build fails with a `static_assert` on a big-endian host.
- **Floats**: IEEE-754 `binary32`, written as their bit pattern via
  `std::bit_cast`.
- **No struct dumping.** Every field is written individually. `sizeof` a struct
  includes alignment padding whose contents are indeterminate and whose layout
  is an ABI property — a different compiler changes it and files stop being
  portable between builds of the same program.
- **Header size is fixed at 64 bytes**, so the payload begins at a 64-byte
  boundary: cache-line and page aligned, and therefore directly memory-mappable.

## Layout

```
offset  size                       field
------  -------------------------  ------------------------------------------
     0  8                          magic "VDBVEC\0\0"
     8  4                          format_version (u32) = 1
    12  4                          dimension (u32), 1..65536
    16  8                          slot_count (u64), includes tombstones
    24  8                          live_count (u64), redundant on purpose
    32  1                          flags (u8): bit 0 = normalized
    33  7                          reserved, zero
    40  8                          payload_bytes (u64), redundant on purpose
    48  4                          payload_crc32 (u32)
    52  8                          reserved, zero
    60  4                          header_crc32 (u32) over bytes [0, 60)
------  -------------------------  ------------------------------------------
    64  slot_count*dimension*4     payload: float32, row-major
     P  slot_count*8               ids:    VectorId (u64) per slot
     I  slot_count*4               norms:  cached L2 norm (f32) per slot
     N  slot_count*1               live:   0 = tombstoned, 1 = live
```

Region offsets are **computed** from the counts, not stored. A stored offset is
one more thing that can disagree with reality; these are trivially derivable and
so cannot.

### Why the regions are separate rather than interleaved

A per-slot record — `{floats, id, norm, live}` — would be the intuitive layout
and it is the wrong one twice over.

1. **It breaks mmap.** With the floats in one contiguous run, the payload region
   *is* `VectorArray`'s in-memory layout, byte for byte, so a load can map the
   file and use the floats in place with no copy and no transformation.
   Interleaving would force a full rewrite pass on every open.
2. **It breaks the scan.** The distance loop reads floats and nothing else. An
   interleaved id would drag 8 bytes of unread data into every cache line the
   loop pulls in.

This is the same "struct of arrays" argument that shaped `VectorStore`, applied
to disk. It is also why
[ADR-0004](decisions/ADR-0004-contiguous-storage.md) refuses to pad the row
stride: padding would fork the memory layout from the file layout and forfeit
exactly this property.

### Why `live_count` and `payload_bytes` are redundant

Both can be recomputed — `live_count` by summing the liveness bytes,
`payload_bytes` as `slot_count × dimension × 4`.

That is the point. **Redundancy is cheap corruption detection.** A header whose
declared payload size disagrees with `slot_count × dimension × 4` is corrupt,
and the check costs one multiply and one comparison. Likewise, a file whose
liveness bytes contain a different number of live slots than the header claims
has been damaged somewhere the structural checks cannot otherwise see.

## Validation on load

In order, cheapest first. Each step's result is only trusted once the previous
one has passed.

1. **File size ≥ 64.** Anything smaller cannot hold a header.
2. **Magic.** Wrong magic means this is not our file; stop before interpreting
   anything as a length.
3. **Format version.** A newer version raises `UnsupportedVersionError` naming
   both versions. Checked *before* the header CRC, so a file from a future
   build reports "too new" rather than the much less helpful "checksum
   mismatch" it would also fail.
4. **Header CRC.** Every remaining field's trustworthiness rests on this, and it
   costs 60 bytes of work.
5. **Field ranges.** `dimension ∈ [1, 65536]`, `slot_count ≤ 2³²−2`,
   `live_count ≤ slot_count`.
6. **Overflow-checked arithmetic.** `slot_count × dimension × 4` is computed
   with an explicit overflow check. A forged header claiming 2⁴⁰ slots of
   dimension 60000 would wrap in 64-bit arithmetic and yield a *small, plausible*
   size — which is precisely how a header that passes validation leads to an
   out-of-bounds read.
7. **Declared payload size** must equal that product.
8. **Actual file size** must equal `total_size()`. This is what catches
   truncation and trailing data, and it distinguishes between them in the error
   message.
9. **Live count** recomputed from the liveness bytes must match the header.

Every failure raises `CorruptionError` (or `UnsupportedVersionError`, which
derives from it) with a message naming the specific invariant that broke.

## What is *not* checked on load

The payload CRC. A full pass over a 3 GB payload takes seconds, and paying it on
every open to catch a class of corruption that would change one distance
slightly is a poor trade.

`vectordb check` runs it explicitly, via `verify_vector_file_payload()`. This is
a documented, deliberate gap rather than an oversight — see
[ADR-0008](decisions/ADR-0008-validate-on-load.md). Per-page checksums verified
on first touch would close it without the full-file cost, and are listed as
future work.

## Writing

Always atomic, via `write_file_atomically`:

```
1. write everything to vectors.bin.tmp
2. fsync(tmp)                — bytes reach the disk, not just the page cache
3. rename(tmp, vectors.bin)  — atomic on POSIX
4. fsync(directory)          — the rename itself reaches the disk
```

A reader ever sees the complete old file or the complete new one. Step 4 is the
one commonly forgotten: `rename` modifies a directory entry, which is separately
cached metadata, so without it a crash can leave the new contents durable while
the directory still points at the old name.

On macOS, step 2 uses `fcntl(F_FULLFSYNC)` rather than `fsync`, because plain
`fsync` there only pushes data as far as the drive's own write cache. Where
`F_FULLFSYNC` is unsupported (some network and virtualised volumes) we fall back
to `fsync` and the weaker guarantee is documented rather than silently assumed.

The writer takes a *list* of byte spans rather than one buffer, so a 3 GB file
never has to be assembled in memory before being written.

## Reading

`read_vector_file` memory-maps the file. Nothing is read until touched; the OS
faults pages in on demand and may evict them under pressure without consulting
us. For a store larger than RAM this is the difference between "cannot open" and
"the working set stays resident".

`madvise(MADV_SEQUENTIAL)` is set for the load, which is a single front-to-back
pass. An HNSW traversal over a mapped file would want `MADV_RANDOM` instead —
read-ahead is wasted bandwidth when access is scattered.

## Size

```
bytes = 64 + slot_count × (dimension × 4 + 8 + 4 + 1)
```

The per-slot overhead is 13 bytes. At `dimension = 768` that is 0.42% of the
3072-byte payload; at `dimension = 4` it is 45%, which is one more reason this
system is designed for embedding-sized vectors.

| slots | dimension | file size |
|---|---|---|
| 10,000 | 128 | 5.0 MB |
| 100,000 | 384 | 155 MB |
| 1,000,000 | 768 | 2.87 GB |
| 1,000,000 | 1536 | 5.73 GB |

## Version history

| Version | Change |
|---|---|
| 1 | Initial format. |
