# Reader-writer concurrency — and a bug that only a test could find

🔴 Advanced · pairs with `src/db/concurrent_database.cpp`

## The shape of the workload

A vector database is built once and queried many times. Searches are read-only
once the graph exists; writes are rare and disruptive.

That asymmetry is worth exploiting, and `std::mutex` does not exploit it:

```cpp
std::mutex mutex;
std::vector<QueryResult> search(...) const {
    std::lock_guard lock(mutex);        // ← every search waits for every other
    return database_.search(...);
}
```

Correct, and it throws away the only easy parallelism you have. Eight cores,
one search at a time.

## `std::shared_mutex`

Two ways to lock it:

```cpp
std::shared_lock lock(mutex_);     // reader: many at once
std::unique_lock lock(mutex_);     // writer: exclusive
```

```
reader ────────────►
reader   ───────────────►      readers never wait for each other
reader ──────►
writer            ·······──────►   waits for readers to drain, then alone
reader                    ·······──────►   waits for the writer
```

The invariant: **any number of readers, or exactly one writer, never both.**

## Where to put the lock

`Database` has no lock at all. `ConcurrentDatabase` wraps it and holds one.

```cpp
class ConcurrentDatabase {
    mutable std::shared_mutex mutex_;
    std::unique_ptr<Database> database_;
};
```

Two reasons for the split:

1. **Single-threaded callers pay nothing.** The CLI, the tests and a bulk import
   never touch a lock. Even an uncontended lock is an atomic read-modify-write
   and a memory barrier — small, but not nothing, and it is on the hot path.
2. **A caller with its own locking is not forced to nest ours inside theirs.**

The `mutable` on the mutex is exactly what `mutable` is for: locking is not part
of the object's observable value, so a `const` method may do it.

## The bug this design could not prevent

The concurrency test failed like this:

```
libc++abi: terminating due to uncaught exception of type vectordb::MetadataError:
  bind integer failed: bad parameter or other API misuse (not an error) [code 21]
```

Eight threads calling `search`, all holding a *shared* lock, all legitimate
readers. And they corrupted each other.

**Because a "read" was not read-only.**

`MetadataStore` keeps its SQLite statements prepared rather than recompiling
them per call — the standard, correct optimisation. But a `sqlite3_stmt` is
**stateful**: it carries a cursor position and its bound parameters. Two threads
stepping the same statement interleave their binds and their steps.

```cpp
Metadata MetadataStore::get(VectorId id) const {
    sql::Statement& statement = *impl_->select_by_vector;   // shared!
    statement.bind_int64(1, id);          // thread A binds 5
                                          // thread B binds 9
    while (statement.step()) { ... }      // both read vector 9
}
```

No reader/writer lock at the *database* level can fix this, because both threads
are readers and the discipline permits exactly what they did.

The fix is that `MetadataStore` synchronises itself, which makes it the one
class in the codebase that breaks the "callers do the locking" rule — with a
comment saying so and why.

### Two lessons worth keeping

**`const` does not mean thread-safe.** It means "does not modify the object's
logical value". A cache, a lazily-computed field, a prepared statement, a memo
table — all are `const`-compatible and all are shared mutable state.

**Reason about what a function *does*, not what its signature says.** The
signature said `const`. The implementation held a cursor.

And it is worth noticing how this was found: not by reading the code, but by a
test that ran eight threads and compared the results with a sequential run. Some
bugs are only visible under contention.

## Choosing the granularity

We considered four options (recorded in
[ADR-0013](../../docs/decisions/ADR-0013-concurrency.md)):

| Approach | Reads | Writes | Complexity |
|---|---|---|---|
| No locking | 1 thread | 1 thread | none |
| One mutex | serialised | serialised | trivial |
| **`shared_mutex`** | **parallel** | **serialised** | **small** |
| Per-node locks | parallel | parallel | large |
| Lock-free + epochs | parallel | parallel | very large |

We took the third. The reasoning is worth generalising: **the fine-grained
options are the right answer once lock contention is measured to be the
bottleneck, and it has not been.**

Under a read-mostly load, shared-lock readers do not contend on the *data* at
all. They contend on the mutex's own cache line — every `shared_lock` is an
atomic increment of a shared counter, so that line ping-pongs between cores.
Real, and much cheaper than the DRAM traffic the search is already doing.

Measure before you go further. The tools are `perf c2c` on Linux and Instruments'
System Trace on macOS.

## Two subtleties in `batch_search`

```cpp
pool_.parallel_for(queries.size(), [&](std::size_t i) {
    const std::shared_lock lock(mutex_);
    results[i] = database_->search(queries[i], options);
});
```

**The lock is taken per query, not around the whole batch.** Holding it outside
would be marginally cheaper — and would let a long batch starve a writer
indefinitely. Most `shared_mutex` implementations only admit a waiting writer at
a moment when no reader holds the lock; with a lock held across a thousand
queries, that moment never comes.

**`results[i]` needs no synchronisation.** The vector is pre-sized, and distinct
elements are distinct objects. Concurrent writes to distinct objects are not a
data race — a rule worth remembering, because the instinct to add a mutex here
is strong and wrong.

(The exception: `std::vector<bool>` is bit-packed, so two "distinct" elements can
share a byte, and writing them concurrently *is* a race.)

## Testing concurrency

Stress tests are necessary and not sufficient.

**Stress test**: eight threads searching while a writer inserts; assert
concurrent results equal sequential ones, and that every returned id resolves.
This is what found the SQLite bug.

**ThreadSanitizer** finds races that did not happen to manifest. It instruments
every memory access and tracks happens-before, so it reports a race the first
time the two accesses occur — even if the timing never actually interleaved
badly.

```sh
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

That distinction matters enormously here. A race in HNSW insertion might corrupt
one neighbour list once in ten thousand runs, and surface months later as an
unexplained recall drop that no test reproduces. TSan finds it on run one.

Our concurrency suite is clean under TSan. The suite was written *first*, and
the bug it found was fixed before the code shipped — which is the whole argument
for writing the stress test before you believe the design.

## What we do not support

**Concurrent writers.** HNSW insertion appends link storage, rewrites the
neighbour lists of existing nodes, and can move the entry point. Making it safe
needs per-node locking with a fixed acquisition order, plus a way to know when a
replaced neighbour list can be freed while a reader may still be walking it.

Not implemented, and — importantly — **not claimed**. The header says so, the
docs say so, and the ADR says what it would take.

An unimplemented feature is a limitation. An undocumented one is a trap.

## Experiments

1. Replace `shared_mutex` with `mutex` and measure batch-search throughput at 1,
   2, 4, 8 threads. The gap is what the reader-writer distinction buys.
2. Remove the internal lock from `MetadataStore` and run the concurrency suite.
   Then run it under `tsan` and compare the diagnostics.
3. Take the shared lock once around the whole batch instead of per query, then
   start a writer while a long batch runs. Time how long the writer waits.
4. Add a deliberate race — two threads incrementing an unsynchronised counter —
   and check whether the stress test catches it. Then check whether TSan does.
5. Measure `shared_lock` acquisition cost with 1 versus 8 threads doing nothing
   else. You are measuring cache-line ping-pong on the mutex itself.

## Where this lives in the code

- `include/vectordb/db/concurrent_database.hpp` — the contract, and what is not
  supported
- `src/db/concurrent_database.cpp` — the locking, and the two `batch_search`
  subtleties
- `include/vectordb/storage/metadata_store.hpp` — the exception to the rule,
  and why
- `tests/integration/concurrent_database_test.cpp` — the stress tests
- `docs/concurrency.md`, `docs/decisions/ADR-0013-concurrency.md`

---

Next: [../70-performance/03-simd-introduction.md](../70-performance/03-simd-introduction.md)
