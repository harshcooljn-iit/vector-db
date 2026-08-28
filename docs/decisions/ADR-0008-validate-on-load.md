# ADR-0008 — Validate every persisted artefact before trusting it

**Status:** Accepted

## Decision

Every file VectorDB reads is validated before its contents are used: magic
number, format version, header self-consistency, and every offset, length and
reference checked against the actual file size and record count. A file that
fails validation raises `CorruptionError` and is never partially applied.

## Context

A database reads files it wrote. It is tempting to treat them as trusted input.
They are not, for reasons that have nothing to do with malice:

- the process was killed between two writes;
- the disk filled mid-write;
- a user copied a database directory while it was open;
- a user pointed `vectordb` at the wrong path;
- a file was written by a newer version with a different layout;
- ordinary bit rot.

## The specific danger

Deserialization code typically reads a length and then allocates it:

```cpp
uint64_t count = read_u64();
std::vector<Node> nodes(count);      // count came from the file
```

If the file is truncated, corrupt, or simply not ours, `count` might be
`0xFFFFFFFFFFFFFFFF`. That line then attempts a 300-exabyte allocation. Best
case `std::bad_alloc`; worse case the OS overcommits and the process is killed
by the OOM reaper; worst case a smaller-but-still-wrong value succeeds and the
subsequent loop reads past the end of the buffer.

The same applies to offsets (`data + offset` past end-of-file) and to graph
references (a neighbour `LocalId` larger than the node count, which turns the
search loop into an out-of-bounds read).

## Options considered

1. **Trust our own files.** Fast, and wrong for all the reasons above.
2. **Checksum everything.** Detects bit rot precisely. Costs a full pass over
   every byte on every open, which for a 3 GB vector file is seconds.
3. **Structural validation, with checksums on the small artefacts.** (chosen)

## Chosen approach

Layered, cheapest first:

1. **Magic + version.** Four bytes say "this is a VectorDB vector file"; a
   version says which layout. Wrong magic → not our file, stop. Newer version →
   `UnsupportedVersionError`, which names both versions.
2. **Header self-consistency.** Dimension within `[1, kMaxDimension]`; count
   within `[0, kMaxVectorCount]`; declared data length equal to
   `count × dimension × 4`; declared length matching the actual file size.
3. **Bounds before allocation.** Any length read from a file is checked against
   the remaining file size *before* it is used to size an allocation. This is
   the rule that makes the exabyte-allocation bug impossible.
4. **Reference validation.** Every `LocalId` in a neighbour list is checked
   against the node count at load time, so the search loop never validates.
5. **Cross-artefact consistency**, in `vectordb check`: index node count equals
   vector count, every metadata row references a live vector, and so on.

Checksums cover the small artefacts (headers, index metadata) where the cost is
negligible. The bulk vector payload is not checksummed on open — see
trade-offs.

## Why

- Corruption caught at load is **recoverable**: the vector store is
  authoritative, so a bad index can be rebuilt. Corruption *not* caught at load
  is a wrong answer with no error attached, forever.
- `CorruptionError` is a distinct type precisely so it can never be handled by a
  generic `catch (...)` that logs and continues.
- Validating references once at load means the inner search loop needs no bounds
  check at all — the validation pays for itself in performance.

## Trade-offs

| Cost | Benefit |
|---|---|
| A validation pass on every open | Corrupt state is detected, not searched |
| Extra header fields (redundant lengths) | Truncation is detectable |
| Bit rot inside the vector payload is not detected on open | Opening a 3 GB database stays fast |

That last row is a real, accepted gap: a flipped bit inside a float will not be
caught by structural validation. It would change one distance slightly and is
about the most benign corruption possible. `vectordb check` computes a full
payload checksum for users who want the guarantee, and it is documented as an
explicit command rather than a hidden cost on every open.

## Consequences

- Every binary format carries magic, version, and redundant length information.
- Deserialization never allocates based on an unchecked file value.
- `vectordb check` exists as a first-class command and reports which invariant
  failed, not just "corrupt".
- A failed index load degrades to "rebuild the index", never to "search anyway".

## Alternatives for future versions

Per-page checksums (CRC32C over each 4 KB page, verified on first touch) would
catch payload bit rot without a full-file pass, and compose well with memory
mapping. This is what real storage engines do.
