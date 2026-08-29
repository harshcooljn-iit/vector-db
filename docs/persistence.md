# Persistence and crash safety

What survives, what does not, and what is guaranteed.

## The artefacts

| File | Role | If lost |
|---|---|---|
| `vectors.bin` | **authoritative** float payload, ids, norms, liveness | data loss |
| `index.hnsw` | derived graph | rebuilt automatically |
| `metadata.sqlite` | metadata rows and database configuration | configuration loss |

Formats: [vector-storage.md](vector-storage.md), [hnsw-format.md](hnsw-format.md),
[metadata.md](metadata.md).

## When things are written

| Operation | Durability |
|---|---|
| `insert`, `remove` (vectors) | **in memory only** until `flush` |
| metadata writes | immediately, in a SQLite transaction |
| `flush` | `vectors.bin` then `index.hnsw`, each atomically |
| destructor | best-effort `flush`, failures logged |
| `rebuild_index`, `compact` | flush as part of the operation |

Rewriting a 3 GB vector file per insert would make bulk loading unusable, so
vectors batch. Metadata is small and transactional, so it does not.

## Atomic replacement

Nothing is ever overwritten in place. Overwriting has a window where the file is
half old and half new, and a crash inside it leaves an unusable database with no
way to tell.

```
1. write everything to vectors.bin.tmp
2. fsync(tmp)                     ← the bytes reach the disk
3. rename(tmp, vectors.bin)       ← atomic on POSIX
4. fsync(directory)               ← the RENAME reaches the disk
```

A reader sees the complete old file or the complete new one.

**Step 4 is the one that gets left out.** `rename` modifies a directory entry,
which is separately cached metadata. Without syncing the directory, a crash can
leave the new contents durable while the directory still points at the old name
— you get the old data back from a write you fsynced.

The order matters too: `fsync(tmp)` must precede the rename, or the directory
can end up pointing at a file whose contents never arrived.

### macOS: `fsync` is not enough

`fsync` pushes data out of the kernel; the drive then accepts it into its own
**volatile write cache** and reports success. A power cut can still lose it.

`fcntl(fd, F_FULLFSYNC)` asks the drive to commit to stable media. It is
dramatically slower and it is what durability actually costs. We use it, and
fall back to `fsync` where it is unsupported (some network and virtualised
volumes) — with the weaker guarantee stated here rather than assumed.

## Flush ordering

`vectors.bin` first, `index.hnsw` second. Deliberately.

A crash between them leaves an index that disagrees with the store, which `open`
detects (node count mismatch) and repairs by rebuilding. The reverse order would
leave an index describing vectors that were never written — the same
inconsistency, but pointing at data that does not exist.

**When you cannot make two writes atomic, order them so the detectable
inconsistency is the recoverable one.**

## What a crash costs you

| Crash point | Result |
|---|---|
| Before `flush` | un-flushed inserts lost; state consistent |
| During the vector write | `.tmp` discarded; previous `vectors.bin` intact |
| Between vector and index write | index rebuilt on next open |
| During the index write | `.tmp` discarded; index rebuilt on next open |
| During a metadata transaction | SQLite rolls it back |
| After `flush` returns | everything durable |

**The guarantee: a crash loses un-flushed inserts and never produces
inconsistent state.** Not "no data loss" — that would require an fsync per
insert. This is the same trade every database makes, stated rather than implied.

## SQLite durability

```sql
PRAGMA journal_mode = WAL;
PRAGMA synchronous  = NORMAL;
```

Durable against **process crash**. Can lose the most recent transactions on a
**power cut**, because `NORMAL` syncs at checkpoints rather than at every
commit.

`FULL` would be safe against both and much slower. `NORMAL` is the standard
choice for a read-mostly workload; the point is that it is a choice, and it is
written down.

## Validation on load

Every file is validated before its contents are used. Nine steps for
`vectors.bin`, cheapest first, each trusted only once the previous passed:
size, magic, version, header CRC, field ranges, overflow-checked arithmetic,
declared payload size, actual file size, recomputed live count.

The index adds a full pass over every neighbour reference — `O(edges)` — which
buys two things: the search loop never bounds-checks a neighbour id, so a corrupt
file cannot become an out-of-bounds read at query time; and a damaged graph is
reported rather than silently returning plausible wrong answers.

Rules that are worth stealing:

- **A length read from a file is checked against the bytes that remain before it
  is used to size anything.** Otherwise a truncated file becomes a 300-exabyte
  allocation.
- **Arithmetic on file-supplied values is overflow-checked.** A header claiming
  2⁴⁰ slots of dimension 60000 wraps in 64-bit arithmetic and produces a small,
  plausible size — which is exactly how a header that passes validation leads to
  an out-of-bounds read. Validation that itself overflows is not validation.
- **Version is checked before the checksum**, so a file from a newer build says
  "written by a newer VectorDB" instead of the equally true but useless
  "checksum mismatch".

[ADR-0008](decisions/ADR-0008-validate-on-load.md).

## What is *not* checked on open

The payload checksums. A full pass over a 3 GB vector file takes seconds, and
paying it on every open to catch corruption that would change one distance
slightly is a poor trade.

`vectordb check --deep` runs them explicitly. A flipped bit inside the float
payload is invisible to structural validation and visible to `--deep`; there are
tests for both halves.

This is a **documented gap**, not an oversight. Per-page checksums verified on
first touch would close it without the full-file cost, and are listed as future
work.

## Recovery

The vector store is authoritative, so a bad index is never a reason to fail an
open:

```
open → load vectors.bin
     → try to load index.hnsw
       ├─ ok, and consistent with the store  → use it       (9 ms at N=50k)
       └─ missing / corrupt / wrong metric   → log, rebuild  (7.8 s at N=50k)
                                             / wrong count
```

The 860× gap between loading and rebuilding is the entire justification for
persisting the index.

The fallback is logged rather than silent — a database that quietly rebuilt on
every open would be a mystery instead of a feature. There are tests for a
truncated index, a garbage index and a missing one.

## No write-ahead log

Considered and not implemented.

A WAL would make individual inserts durable without rewriting the vector file,
which is the one real gap in the current model. It needs a log format, a replay
path, checkpointing, and a recovery routine that is exercised often enough to be
trustworthy — which is a phase of its own.

The current model is honest about what it gives: batch durability at `flush`,
with a consistency guarantee in between. Listed in
[`learnings/next-steps.md`](../learnings/next-steps.md).

## Verifying it yourself

```sh
vectordb check mydb          # structure, cross-artefact consistency
vectordb check mydb --deep   # + payload checksums, + NaN scan
```

Exit code 3 means "the check ran and found errors" — distinct from 1, so a
script can tell the two apart.
