# Consistency — what your database promises

🔴 Advanced

## The question nobody asks until it is too late

The process dies mid-write. What state is the database in?

There are three possible answers, and only one of them is acceptable:

1. **Consistent, all changes applied.**
2. **Consistent, some changes lost.**
3. **Inconsistent.**

(1) and (2) are both fine — the second is a *durability* limitation, and you can
tell users about it. (3) is unacceptable, because there is no way to know it
happened and no way to recover.

> Durability is a dial you can set. Consistency is a property you must not lose.

## What VectorDB promises

> **A crash loses un-flushed inserts and never produces inconsistent state.**

Not "no data loss". Vectors are durable at `flush`, not per insert. Rewriting a
3 GB file per vector would make bulk loading unusable, so the trade is made
deliberately — and written down rather than implied.

| Crash point | Result |
|---|---|
| Before `flush` | un-flushed inserts lost; consistent |
| During the vector write | `.tmp` discarded; previous file intact |
| Between vector and index write | index rebuilt on next open |
| During the index write | `.tmp` discarded; index rebuilt |
| During a metadata transaction | SQLite rolls it back |
| After `flush` returns | durable |

## The three mechanisms

### 1. Atomic replacement

Nothing is overwritten in place. Write to `.tmp`, fsync, rename, fsync the
directory. A reader sees the complete old file or the complete new one.

The failure this prevents: overwriting has a window where the file is half old
and half new, and a crash inside it leaves an unusable database **with no way to
tell**.

### 2. Ordering

`vectors.bin` first, `index.hnsw` second.

A crash between them leaves an index disagreeing with the store — detectable at
open (node count mismatch) and recoverable (rebuild). The reverse order would
leave an index describing vectors that were never written: the same
inconsistency, but pointing at data that does not exist.

> **When you cannot make two writes atomic, order them so the detectable
> inconsistency is the recoverable one.**

This is the single most portable idea on this page.

### 3. Authoritative versus derived

`vectors.bin` is authoritative. `index.hnsw` is derived.

So there is always a direction for reconciliation: when they disagree, the vector
store wins and the index is rebuilt. No ambiguity, no merge, no guessing.

A system where two artefacts are both authoritative and can disagree has no
recovery story — only a choice about which corruption to keep.

## The gap we accept

Metadata is written immediately; vectors are written at `flush`. A crash between
`database.insert(id, v, meta)` and `flush()` leaves a metadata row whose vector
does not exist.

Those rows are **inert** — nothing reads metadata for an absent vector — so this
is a tidiness problem, not a correctness one. And it is handled rather than
ignored: `check` reports orphaned rows, `compact` removes them.

**Naming a gap and bounding its consequences is a legitimate engineering answer.
Pretending it does not exist is not.**

## What we do not have

**Transactions across the whole database.** You cannot atomically insert 1,000
vectors such that a crash leaves all or none. Metadata gets that from SQLite;
vectors do not.

**Per-insert durability.** That needs a write-ahead log — a format, a replay
path, checkpointing, and a recovery routine exercised often enough to trust.
Listed in [next-steps](../next-steps.md).

**Isolation between concurrent writers.** Writes are serialised by an exclusive
lock, so isolation is trivially total, but that is a consequence of not supporting
concurrent writers rather than a feature.

## Reading the ACID letters honestly

| | VectorDB |
|---|---|
| **Atomicity** | per file (atomic replacement); per metadata transaction. Not across the database. |
| **Consistency** | yes — invariants hold after any crash, validated on load |
| **Isolation** | serialised writers, so total. Readers see a consistent snapshot per operation. |
| **Durability** | at `flush`, not per insert. macOS uses `F_FULLFSYNC`. |

A useful exercise for any system: fill in this table and refuse to write "yes"
without saying *at what granularity*.

## Experiments

1. Insert 1,000 vectors without flushing, then `kill -9`. Reopen. How many
   survive? Is the database usable?
2. Kill the process during a `flush` (a `sleep` in `write_file_atomically` will
   help). Confirm the old file is intact and no `.tmp` remains after reopen.
3. Insert with metadata, kill before flush, reopen, and run `check`. Find the
   orphan warning.
4. Reverse the flush order — index before vectors — and construct the crash that
   makes it worse.
5. Write down the ACID table for a system you use daily. Check the documentation.
   Note how often the granularity is unstated.

## Where this lives in the code

- `src/persistence/file_io.cpp` — `write_file_atomically`
- `src/db/database.cpp` — `flush`'s ordering, `open`'s recovery, `check`
- `docs/persistence.md`

---

Next: [02-crash-safety.md](02-crash-safety.md)
