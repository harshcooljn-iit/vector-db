# Race conditions and mutexes

🟡 Intermediate

## What a data race actually is

```cpp
int counter = 0;
// two threads:
counter++;
```

`counter++` is three operations: load, add, store. Interleave them and one
increment vanishes.

But it is worse than "sometimes you lose an update". A data race is **undefined
behaviour** in C++, which means the compiler may assume it does not happen and
optimise accordingly — hoisting the load out of a loop, for instance, so a thread
never sees another's write *at all*.

> A data race is not "the answer might be wrong". It is "the program has no
> defined meaning."

## The definition

A data race requires all four:

1. two or more threads access the same memory location
2. at least one access is a **write**
3. the accesses are not ordered by a synchronisation relationship
4. they are not all atomic

Break any one and there is no race. Note (2): **concurrent reads are always
safe**, which is the entire basis for VectorDB's reader-writer design.

## Mutexes

```cpp
std::mutex mutex;
{
    std::lock_guard<std::mutex> lock(mutex);   // RAII: unlocks on every path
    counter++;
}
```

Mutual exclusion: at most one thread inside at a time.

**Always use a guard, never `lock()`/`unlock()` by hand.** An early return or an
exception between them leaves the mutex held forever, and the next thread waits
for a lock that will never be released.

| Guard | Use |
|---|---|
| `std::lock_guard` | simplest, most cases |
| `std::unique_lock` | when you need to unlock early, or with a condition variable |
| `std::scoped_lock` | two or more mutexes at once, deadlock-free |

## Deadlock

```cpp
// thread 1                    // thread 2
lock(a); lock(b);              lock(b); lock(a);      // ☠️
```

Each holds what the other wants.

The standard prevention is a **global lock ordering**: always acquire in the same
order everywhere. Or `std::scoped_lock(a, b)`, which uses a deadlock-free
algorithm.

VectorDB mostly sidesteps this by never holding two locks at once. The one place
it would arise — `MetadataStore`'s internal mutex being taken while a caller
holds the database's — is safe because the ordering is always the same direction
(database first, metadata second) and never the reverse.

## What a mutex costs

**Uncontended**: an atomic read-modify-write plus a memory barrier. Tens of
nanoseconds. Small but not free — which is why `Database` has no lock at all and
`ConcurrentDatabase` adds one only for callers who need it.

**Contended**: a system call, a context switch, a cache line ping-ponging between
cores. Microseconds. This is where scalability dies.

The measured example is in
[05-reader-writer-concurrency](05-reader-writer-concurrency.md): a mutex taken
ten times per query turned a search that scaled 4.3× into one that peaked at
1.8×.

## `const` does not mean thread-safe

The most valuable sentence on this page.

`const` means "does not modify the object's *logical* value". A cache, a memo
table, a lazily computed field, or a **SQLite prepared statement** are all
`const`-compatible shared mutable state.

`MetadataStore::get` is `const`. It mutates a prepared statement's cursor and
bound parameters. Eight concurrent readers corrupted each other, and no
caller-side reader/writer lock could have prevented it.

**Reason about what a function does, not what its signature says.**

## Minimise the critical section

```cpp
{
    std::lock_guard lock(mutex_);
    task = std::move(tasks_.front());   // just the shared bit
    tasks_.pop();
}
task();                                 // ← outside the lock
```

Running the task inside would serialise every task and make the pool an
elaborate way to run things one at a time. It would still *work*, which is what
makes it an easy mistake.

## Experiments

1. Increment a shared counter from 8 threads without a mutex, 1,000,000 times
   each. Report the final value. Run it ten times.
2. Now do it under `tsan` and read the report.
3. Replace the counter with `std::atomic<int>` and compare correctness and speed.
4. Construct a deadlock with two mutexes acquired in opposite orders. Then fix it
   with `std::scoped_lock`.
5. Move `task()` inside the lock in `worker_loop` and measure the batch benchmark
   at 8 threads.

## Where this lives in the code

- `src/concurrency/thread_pool.cpp` — a minimal critical section
- `src/storage/metadata_store.cpp` — the internal mutex, and why it must exist
- `src/db/concurrent_database.cpp` — reader/writer locking

---

Next: [03-condition-variables.md](03-condition-variables.md)
