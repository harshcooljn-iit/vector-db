# Persistent storage — what "saved" means

🟡 Intermediate

## The question

Your program has a million vectors in memory. The process exits. What do you
have?

Nothing, unless you did something. Making data outlive a process is the entire
subject of storage engineering, and it is harder than "write it to a file".

## Three things a storage layer must do

1. **Represent** — turn in-memory values into bytes, and back.
   → [02-binary-file-formats](02-binary-file-formats.md)
2. **Persist durably** — get those bytes onto stable media, and survive a crash
   part-way through.
   → [../10-cpp-systems-foundations/06-system-calls](../10-cpp-systems-foundations/06-system-calls.md)
3. **Validate** — never trust a file, even one you wrote.
   → [ADR-0008](../../docs/decisions/ADR-0008-validate-on-load.md)

Skipping any one gives you a system that works until it does not.

## Authoritative versus derived

The most important idea in VectorDB's storage design:

```
vectors.bin       AUTHORITATIVE   losing it is data loss
index.hnsw        derived         rebuild it in 7.8 s
metadata.sqlite   authoritative   for metadata and config
```

That asymmetry decides everything downstream:

- A corrupt index is **never** a reason to fail an open. It is a reason to spend
  CPU.
- `flush` writes vectors first, index second — so a crash between them leaves the
  detectable-and-recoverable inconsistency rather than the other one.
- `check` treats an index problem as an error with a fix, and an orphaned
  metadata row as a warning, because nothing reads it.

**Ask of every artefact: if I lost this, could I reconstruct it?** The answer
changes how much you must protect it.

## When to write

| Strategy | Durability | Cost |
|---|---|---|
| Every insert | high | rewrite 3 GB per vector — unusable |
| On `flush` | batch | what we do for vectors |
| Transactional | per-operation | what SQLite gives us for metadata |
| WAL | per-operation, cheap | not implemented |

VectorDB's guarantee, stated rather than implied:

> A crash loses un-flushed inserts and **never produces inconsistent state.**

Not "no data loss" — that needs an fsync per insert, or a WAL. Every database
makes this trade; the difference is whether it tells you.

## Why not just serialize the whole object graph?

Tempting: one `save()`, one `load()`.

It fails on three counts, each fatal at scale:

- **You must load everything to read anything.** A 3 GB database cannot be opened
  on an 8 GB machine alongside anything else.
- **You must write everything to change anything.** One insert rewrites 3 GB.
- **Partial corruption is total loss.** One bad byte and the whole graph is
  suspect.

Real storage engines use fixed-size records at computable offsets, which is why
`vectors.bin` is a header plus flat arrays: slot `i` is at a known offset, and
the payload region can be memory-mapped and used in place.

## Why the layouts match

The float payload in `vectors.bin` **is** `VectorArray`'s memory layout, byte for
byte. That is not a coincidence — it is what
[ADR-0004](../../docs/decisions/ADR-0004-contiguous-storage.md) gave up padding
to preserve, and it is what makes `mmap` useful rather than decorative.

A format that needed transformation on load would force a full pass over 3 GB
every time you opened the database.

## Where this lives in the code

- `src/storage/vector_file.cpp` — the format, the validation ladder
- `src/persistence/file_io.cpp` — atomic writes, `mmap`
- `src/db/database.cpp` — `flush`, `open`'s recovery path
- `docs/persistence.md`, `docs/vector-storage.md`

---

Next: [02-binary-file-formats.md](02-binary-file-formats.md)
