# Corruption — never trust a file, including your own

🔴 Advanced

## Files go wrong without malice

- the process was killed between two writes
- the disk filled mid-write
- someone copied the directory while it was open
- someone pointed the tool at the wrong path
- the file was written by a newer version with a different layout
- ordinary bit rot

None of these require an attacker. All of them produce a file your code will
happily read.

## The bug this prevents

```cpp
std::uint64_t count = read_u64();
std::vector<Node> nodes(count);        // ☠️
```

`count` came from a file. A truncated or foreign file might make it
`0xFFFFFFFFFFFFFFFF`, and that line attempts a **300-exabyte allocation**.

Best case `bad_alloc`. Worse, the OS overcommits and the OOM killer arrives.
Worst, a smaller-but-still-wrong value succeeds and the following loop reads past
the end.

> **A length read from a file is checked against the bytes that actually remain,
> before it is used to size anything.**

That is the rule. Everything else on this page is an application of it.

## Validation, cheapest first

Each step's result is only trusted once the previous has passed.

| Step | Cost | Catches |
|---|---|---|
| 1. size ≥ header size | free | a stub file |
| 2. magic | free | not our file |
| 3. format version | free | written by a newer build |
| 4. header CRC | 60 bytes | a damaged header |
| 5. field ranges | free | absurd dimension or count |
| 6. overflow-checked arithmetic | free | a forged header |
| 7. declared size vs computed | free | internal inconsistency |
| 8. actual file size | one `stat` | truncation, trailing data |
| 9. recomputed live count | O(N) bytes | subtle damage |
| 10. reference validation (index) | O(edges) | dangling edges |

## Three details worth stealing

### Version before checksum

A file from a newer build fails *both*. Reporting the version first gives the
user "written by a newer VectorDB" instead of "checksum mismatch", and only one
of those tells them what to do.

**When several checks would fail, report the most actionable one.**

### Overflow-checked arithmetic

```cpp
std::uint64_t expected = slot_count * dimension * 4;   // ☠️
```

A forged header claiming `slot_count = 2^40`, `dimension = 60000` produces a true
product of ~2.4 × 10¹⁷ × 4, which **wraps** in 64-bit arithmetic to a small,
plausible number. The size check then passes, and you read from a region that
does not exist.

**Validation that itself overflows is not validation.**

### Redundant fields

`live_count` is derivable by summing the liveness bytes; `payload_bytes` is
`slot_count × dimension × 4`. Both are stored anyway.

That redundancy is free corruption detection: a mismatch costs one comparison to
find and would otherwise be invisible.

## What we deliberately do not check on open

Payload checksums.

A full pass over 3 GB takes seconds, to catch corruption that would shift one
distance slightly. `vectordb check --deep` runs it explicitly.

This is a **documented gap**, not an oversight — and there are tests for both
halves: a flipped payload byte still loads, and `--deep` catches it. Per-page
checksums verified on first touch would close it without the full-file cost, and
are listed as future work.

**A gap you have named, bounded and given a command for is engineering. A gap you
have not noticed is a bug.**

## Errors must be actionable

```
hnsw index: node 47 layer 0 references node 99999, outside [0, 5000)
hnsw index was built for metric 'cosine' but this database uses 'l2';
    rebuild the index
vector file: header describes 819264 bytes but the file is 819200 — the file
    is truncated
```

Each names the artefact, the specific invariant, the values involved, and — where
there is one — the fix.

Compare with "corrupt file", which tells a user nothing they can act on.

## Recovery beats detection

Detecting corruption is table stakes. Doing something about it is the feature.

Because `vectors.bin` is authoritative and `index.hnsw` is derived, a corrupt
index is **never** a reason to fail an open. It is logged and rebuilt. There are
tests for a truncated index, a garbage index and a missing one; all three reopen
and search correctly.

**Design so that the artefact most likely to be corrupt is the one you can
rebuild.**

## Experiments

1. Work through [Exercise 9](../100-exercises/09-corrupt-a-file.md) — corrupt
   each header field in turn and predict the message before reading it.
2. Remove the overflow check and hand-craft a header with
   `slot_count = 2^40, dimension = 60000`.
3. Flip a bit in the payload. Confirm `check` misses it and `check --deep` finds
   it. Decide whether you agree with the default.
4. Find a single-byte change VectorDB does not detect that changes a search
   result. Design the cheapest mechanism that would catch it.
5. Reverse the version and CRC checks and note how the error message degrades.

## Where this lives in the code

- `src/storage/vector_file.cpp` — `parse_vector_file_header`
- `src/index/hnsw/hnsw_file.cpp` — header plus reference validation
- `src/persistence/byte_io.cpp` — `ByteReader::require`
- `src/db/database.cpp` — `check`, and the open-time rebuild
- `docs/decisions/ADR-0008-validate-on-load.md`

---

Next: [04-index-rebuilding.md](04-index-rebuilding.md)
