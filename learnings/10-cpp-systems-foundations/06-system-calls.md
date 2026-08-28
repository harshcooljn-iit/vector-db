# System calls — and what "saved" actually means

🟡 Intermediate → 🔴 Advanced · pairs with `src/persistence/file_io.cpp`

## What a syscall is

Your process cannot touch the disk. It cannot touch the network, or another
process's memory. Those are the kernel's, and the CPU enforces the separation in
hardware.

A **system call** is the doorway: a controlled transition into kernel mode, the
kernel does the privileged thing, and control returns.

```
your code            kernel
---------            ------
write(fd, ...) ────> validate fd, copy bytes into the page cache
               <──── return byte count
```

It is not a function call. It costs hundreds of nanoseconds to a few
microseconds — cheap next to a disk, expensive next to a loop iteration. That
ratio explains most I/O design: **batch your syscalls.** Writing 3 GB in one
`write` beats a million 3 KB writes not because of the bytes, but because of the
999,999 doorway transitions.

## The five this project uses

| Call | Does |
|---|---|
| `open` | resolve a path, return a file descriptor |
| `write` | copy bytes from your memory into the kernel's page cache |
| `fsync` | force those cached bytes onto the physical device |
| `rename` | atomically repoint a directory entry |
| `mmap` | map a file into your address space |

## The trap: `write` does not write

```cpp
write(fd, data, size);      // returns
// crash here
```

Your data may be gone.

`write` copies into the kernel's **page cache** and returns. The kernel flushes
to disk when it feels like it — often seconds later. That is why the second
`grep` of a large file is instant, and it is why a power cut can lose data your
program was told it had saved.

`fsync(fd)` is the barrier that says "do it now, and do not return until it is
done".

### On macOS, `fsync` is not enough either

`fsync` pushes data out of the kernel, and the *drive* accepts it into its own
volatile write cache and reports success. A power cut can still lose it.

`fcntl(fd, F_FULLFSYNC)` asks the drive to commit to stable media. It is
dramatically slower and it is what durability actually costs.

```cpp
#if defined(__APPLE__)
    if (fcntl(fd, F_FULLFSYNC) == -1) { if (fsync(fd) == -1) throw ...; }
#else
    if (fsync(fd) == -1) throw ...;
#endif
```

Note the fallback: not every filesystem implements `F_FULLFSYNC` (network
mounts, some virtualised volumes). Falling back is better than failing the
write, and the weaker guarantee is written down in `docs/persistence.md` rather
than quietly assumed.

## `write` is allowed to be lazy

```cpp
ssize_t n = write(fd, data, size);
// n may be less than size, and that is not an error
```

A **short write** is legal: on a signal, on a pipe, on some filesystems for very
large writes. Treating it as success loses the tail of your file.

It is also nearly invisible in testing, because small writes almost never come
up short. So the loop is mandatory:

```cpp
while (written < size) {
    ssize_t r = write(fd, data + written, size - written);
    if (r < 0) {
        if (errno == EINTR) continue;   // a signal arrived; retry, not an error
        throw IoError::from_errno("write", path, errno);
    }
    written += r;
}
```

`EINTR` deserves its own mention: a signal arriving mid-syscall makes it return
`-1` with `errno == EINTR`, meaning "nothing went wrong, try again". Code that
treats every `-1` as fatal breaks mysteriously under a debugger or a profiler,
which send signals.

## Atomic replacement, and the step everyone forgets

We never overwrite a file in place. Overwriting has a window where the file is
half old and half new, and a crash inside that window leaves an unusable
database with no way to tell.

```
1. write everything to vectors.bin.tmp
2. fsync(tmp)                     ← bytes are durable
3. rename(tmp, vectors.bin)       ← atomic: one or the other, never both
4. fsync(directory)               ← the RENAME is durable
```

`rename` over an existing file is atomic on POSIX: any observer sees the old
file or the new one, never a mixture.

**Step 4 is the one that gets left out.** `rename` modifies a *directory entry*,
which is metadata living in its own blocks with its own caching. Without syncing
the directory, a crash can leave the new file's contents on disk while the
directory still points at the old name — and you get the old data back from a
write you fsynced.

The order matters too. `fsync(tmp)` before `rename` is essential: reverse them
and you can end up with the directory pointing at a file whose contents never
arrived.

## RAII over a descriptor, and why the destructor is not enough

```cpp
class FileDescriptor {
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    void close();   // explicit, and it throws
};
```

The destructor guarantees the descriptor is released on every path, including an
exception mid-write. Without it, a throw between `open` and `close` leaks a
descriptor, and a long-running process eventually hits `EMFILE`.

But note there are *two* ways to close. The destructor cannot report failure — a
throwing destructor during stack unwinding calls `std::terminate` — and `close`
**can** fail: on some filesystems it is where a deferred write error finally
surfaces.

So the atomic-write path calls `file.close()` explicitly and lets it throw. The
destructor is the safety net for the error paths, not the mechanism for the
happy path.

That distinction generalises: **RAII guarantees release, not success.** When
success matters, ask for it explicitly.

## Errors carry context or they carry nothing

```cpp
throw IoError::from_errno("open for write", path, errno);
// "open for write failed for '/data/papers/vectors.bin.tmp': Permission denied"
```

versus `throw IoError("failed")`, which tells a user nothing they can act on.

Two details: capture `errno` *immediately* (the next library call may overwrite
it), and use `strerror_r` rather than `strerror`, which is not thread-safe.

## Experiments

1. Write 100 MB with and without `fsync`, and time both. The ratio is what
   durability costs. On macOS, add `F_FULLFSYNC` as a third case.
2. Write a file with one `write(3 GB)` and with a million `write(3 KB)`. Time
   both, then use `strace -c` (Linux) or `dtruss -c` (macOS) to count syscalls.
3. Delete the `EINTR` retry, then run the program under a profiler that samples
   with signals. Watch it fail intermittently.
4. Write to a temp file, `fsync` it, `rename` it, and *skip* the directory
   `fsync`. On a filesystem you do not mind hurting, pull the plug. (Or read the
   ext4 and APFS documentation on directory metadata journalling and convince
   yourself on paper.)
5. Comment out `~FileDescriptor`, then run a loop that opens a file and throws.
   Watch the descriptor count climb with `lsof`, then hit `EMFILE`.

## Where this lives in the code

- `include/vectordb/persistence/file_io.hpp` — `FileDescriptor`, the atomic
  write contract, `MappedFile`
- `src/persistence/file_io.cpp` — `write_all` with its `EINTR` loop,
  `fsync_or_throw` with the `F_FULLFSYNC` path, `fsync_directory`
- `src/core/error.cpp` — `IoError::from_errno`
- `docs/persistence.md` — the durability guarantees, stated honestly

---

Next: [../50-storage/01-persistent-storage.md](../50-storage/01-persistent-storage.md)
