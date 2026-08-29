# RAII — and what it does not guarantee

🟢 Foundational → 🟡 the nuance is intermediate

## The idea

Tie a resource's lifetime to an object's. The destructor releases it, on every
path — normal return, early return, or an exception thrown three frames down.

```cpp
class FileDescriptor {
    int fd_ = -1;
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    // copying would double-close; moving is fine
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
};
```

`std::exchange` in the move constructor is the whole trick: take the value, leave
`-1` behind, so the moved-from object's destructor does nothing.

Without this, a `throw` between `open` and `close` leaks a descriptor, and a
long-running process eventually hits `EMFILE`.

## What VectorDB wraps

| Wrapper | Resource | Released by |
|---|---|---|
| `FileDescriptor` | an OS file descriptor | `close` |
| `MappedFile` | a memory mapping | `munmap` |
| `sql::Connection` | a SQLite handle | `sqlite3_close_v2` |
| `sql::Statement` | a prepared statement | `sqlite3_finalize` |
| `sql::Transaction` | an open transaction | **rollback** |
| `std::lock_guard` | a mutex | unlock |
| `TempDir` (tests) | a directory | `remove_all` |

## The nuance: RAII guarantees release, not success

```cpp
class FileDescriptor {
    ~FileDescriptor();   // cannot report failure
    void close();        // explicit, and throws
};
```

A destructor **must not throw** — one thrown during stack unwinding calls
`std::terminate`. So a destructor cannot tell you that `close` failed.

And `close` genuinely can fail. On some filesystems it is where a deferred write
error finally surfaces. Silently ignoring that in a database would mean reporting
a successful write that did not happen.

So the atomic-write path calls `file.close()` **explicitly** and lets it throw.
The destructor is the safety net for error paths, not the mechanism for the
happy one.

> **When success matters, ask for it explicitly. RAII is for cleanup, not for
> confirmation.**

## Which way should the default point?

```cpp
~Transaction() { if (!finished_) rollback(); }
```

Rollback is what happens if you do nothing. You must *ask* for the change to
persist.

The opposite default — commit on destruction — turns every forgotten error path
into silent partial data. **Make the safe outcome the one that requires no
action.**

## The rule of five, in practice

Declare a destructor and you probably need to think about copy and move.
VectorDB's classes almost all land on:

```cpp
Thing(const Thing&) = delete;             // copying would double-release
Thing& operator=(const Thing&) = delete;
Thing(Thing&&) noexcept;                  // moving is fine
Thing& operator=(Thing&&) noexcept;
```

`noexcept` on the move operations is not decoration: `std::vector` will *copy*
instead of moving during reallocation if the move constructor might throw.

## The pimpl idiom, and why `Database` uses it

```cpp
class Database {
    class Impl;
    std::unique_ptr<Impl> impl_;
public:
    ~Database();               // must be defined in the .cpp
};
```

`Impl` is defined only in `database.cpp`, so the header does not need to include
`VectorStore`, `MetadataStore` or `HnswIndex`. Consumers get faster compiles and
cannot depend on internals.

The catch: the destructor must be defined where `Impl` is complete, or
`unique_ptr` cannot delete it. Hence `~Database();` declared in the header and
`= default` in the source. It is a confusing error the first time you meet it.

## What RAII does not cover

**Circular `shared_ptr`s** never release. VectorDB uses `unique_ptr` almost
exclusively, which sidesteps this.

**Resources shared across threads** need more than a destructor — see
[60-concurrency/05](../60-concurrency/05-reader-writer-concurrency.md).

**Ordering between independent objects** is not guaranteed beyond reverse
declaration order within a scope. When order matters, be explicit.

## Experiments

1. Remove `~FileDescriptor`, then loop opening a file and throwing. Watch
   descriptors climb with `lsof`, then hit `EMFILE`.
2. Remove `std::exchange` from the move constructor and move a `FileDescriptor`.
   You now have a double close.
3. Make `~Transaction` commit instead of rolling back. Run
   `MetadataStore.RollsBackAFailedTransaction`.
4. Delete `~Database()` from the header and see what the compiler says about
   `unique_ptr<Impl>`. It is worth meeting that error once deliberately.
5. Make a move constructor non-`noexcept` and watch `std::vector` copy instead of
   move on reallocation.

## Where this lives in the code

- `include/vectordb/persistence/file_io.hpp` — `FileDescriptor`, `MappedFile`
- `src/storage/sqlite.hpp` — `Connection`, `Statement`, `Transaction`
- `include/vectordb/db/database.hpp` — pimpl, move-only
- `tests/support/temp_dir.hpp` — RAII in a test helper

---

Next: [03-memory-layout.md](03-memory-layout.md)
