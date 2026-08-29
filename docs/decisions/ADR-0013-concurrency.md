# ADR-0013 — A reader-writer lock, held by an opt-in wrapper

**Status:** Accepted

## Decision

Concurrent searches are supported. Concurrent writes are not. `Database` carries
no lock at all; `ConcurrentDatabase` wraps it in a `std::shared_mutex` and is
the class a multi-threaded caller uses.

## Context

Vector search is read-mostly: an index is built once and queried many times.
Search is the operation worth parallelising, and it is naturally read-only once
the graph exists.

Writes are the opposite. HNSW insertion appends link storage, rewrites the
neighbour lists of existing nodes, and can move the entry point.

## Options considered

1. **No concurrency.** Correct, and leaves the machine's other seven cores idle
   on the one operation that matters.
2. **A single mutex around everything.** Correct, and serialises searches for no
   reason — the exact case a reader-writer lock exists for.
3. **`shared_mutex`: many readers, one writer.** (chosen)
4. **Lock-free reads with epoch-based reclamation.** The fastest option and a
   large undertaking.
5. **Fine-grained per-node locking, allowing concurrent writers.** What a
   production system eventually does.

## Chosen approach

Option 3, with the lock in a separate wrapper class rather than in `Database`.

## Why

- **It matches the workload.** Readers do not block readers, which is where all
  the available parallelism is.
- **It is small enough to be obviously correct.** Roughly 80 lines, no
  reclamation scheme, no memory-ordering subtleties.
- **The wrapper split means single-threaded callers pay nothing.** The CLI, the
  tests and a bulk import never touch a lock, and a caller with its own locking
  scheme is not forced to nest ours inside theirs.

Options 4 and 5 were rejected on the same grounds: they are the right answer
once *lock contention* is measured to be the bottleneck, and it has not been.
Under a read-mostly load, shared-lock readers do not contend with each other on
the data — they contend on the mutex's own cache line, which is much cheaper
than the DRAM traffic the search is already doing.

## Trade-offs

| Cost | Benefit |
|---|---|
| No concurrent writers | A small, verifiable implementation |
| A long batch could starve a writer | Reads scale to the core count |
| Two classes instead of one | Single-threaded callers pay nothing |
| Readers still contend on the mutex's cache line | No reclamation scheme to get wrong |

The starvation risk is mitigated: batch search takes a shared lock *per query*
rather than one across the whole batch, so a waiting writer gets a chance
between queries.

## Consequences

- `Database`'s header documents the contract; `ConcurrentDatabase` enforces it.
  Documented-but-unenforced would be a trap, so the enforcing class exists.
- `HnswIndex::search` keeps its scratch in `thread_local` storage, because a
  `const` method under concurrent readers cannot use shared mutable scratch.
- **`MetadataStore` breaks the pattern and locks itself.** Its reads are not
  read-only: prepared statements are reused and carry a cursor, so two
  concurrent readers corrupt each other. Caller-side reader/writer locking
  cannot fix that, because both threads are legitimate readers. Found by a
  test, not by reasoning.
- The `tsan` preset exists and the concurrency suite is clean under it. A race
  that does not manifest is still a race.

## Alternatives for future versions

- **Concurrent inserts**, via per-node locks with a fixed acquisition order plus
  a reclamation scheme for replaced neighbour lists.
- **A sharded index**, where each shard has its own lock and a query fans out.
  Much simpler than fine-grained locking and often enough in practice.
- **Copy-on-write index swapping**: build a new graph in the background, publish
  it with one atomic pointer store. Gives lock-free reads without any
  reclamation subtlety, at the cost of two full copies during a rebuild.
