# Crash safety — the four-step write

🔴 Advanced

## `write()` does not write

```cpp
write(fd, data, size);
// crash here
```

Your data may be gone.

`write` copies into the kernel's **page cache** and returns. The kernel flushes
when it feels like it — often seconds later. That is why the second `grep` of a
large file is instant, and why a power cut loses data your program was told it
had saved.

## The sequence

```
1. write everything to vectors.bin.tmp
2. fsync(tmp)                     ← bytes reach the device
3. rename(tmp, vectors.bin)       ← atomic on POSIX
4. fsync(directory)               ← the RENAME reaches the device
```

Each step earns its place.

**Step 1 — a temporary.** Never overwrite in place. Overwriting has a window
where the file is half old and half new, and a crash inside it is unrecoverable
and undetectable.

**Step 2 — before the rename.** Reverse them and you can end up with a directory
entry pointing at a file whose contents never arrived. This ordering is the
whole point.

**Step 3 — `rename` is atomic.** Any observer sees the old file or the new one.
Guaranteed by POSIX for a rename within one filesystem.

**Step 4 — the one everybody forgets.** `rename` modifies a *directory entry*,
which is metadata living in its own blocks with its own caching. Without syncing
the directory, a crash can leave the new contents durable while the directory
still points at the old name — you get the old data back from a write you
fsynced.

## macOS: `fsync` is not enough

`fsync` pushes data out of the kernel. The **drive** then accepts it into its own
volatile write cache and reports success. A power cut can still lose it.

```cpp
#if defined(__APPLE__)
    if (fcntl(fd, F_FULLFSYNC) == -1) { if (fsync(fd) == -1) throw ...; }
#else
    if (fsync(fd) == -1) throw ...;
#endif
```

`F_FULLFSYNC` asks the drive to commit to stable media. It is dramatically
slower, and it is what durability actually costs.

Note the fallback: not every filesystem implements it (network mounts, some
virtualised volumes). Falling back is better than failing the write, and the
weaker guarantee is documented rather than silently assumed.

## Short writes

```cpp
ssize_t n = write(fd, data, size);   // n may be < size, and that is not an error
```

Legal on a signal, on a pipe, on some filesystems for very large writes. Treating
it as success loses the tail of your file — and it is nearly invisible in
testing, because small writes almost never come up short.

```cpp
while (written < size) {
    ssize_t r = write(fd, data + written, size - written);
    if (r < 0) { if (errno == EINTR) continue; throw ...; }
    written += r;
}
```

`EINTR` means "a signal arrived, nothing went wrong, try again". Code that treats
every `-1` as fatal breaks under any profiler or debugger that sends signals.

## Testing crash safety

Genuinely hard, because you must crash at the right moment.

| Technique | What it gives |
|---|---|
| `kill -9` at random points | cheap; catches gross errors |
| A `sleep` between steps, then kill | targets a specific window |
| `LD_PRELOAD` an `fsync` that lies | simulates a lying drive |
| A filesystem-level fault injector | thorough; a project in itself |
| Reasoning from the specification | what we mostly rely on |

VectorDB tests what it can: atomic replacement leaves no `.tmp`, a truncated file
is rejected, a corrupt index is rebuilt. The rest is argued from POSIX semantics
and written down so a reader can check the argument.

**Being honest about which guarantees are tested and which are reasoned is part
of the guarantee.**

## Experiments

1. Write 100 MB with and without `fsync`, and time both. On macOS add
   `F_FULLFSYNC` as a third case. That ratio is what durability costs.
2. Insert without flushing, `kill -9`, reopen. Then flush and repeat.
3. Add a `sleep(5)` between the rename and the directory fsync, kill during it,
   and reason about what could have been lost.
4. Delete the `EINTR` retry and run under a sampling profiler.
5. Read your filesystem's documentation on directory metadata journalling
   (ext4's `data=ordered`, APFS's copy-on-write). Decide whether step 4 is
   strictly necessary there — and whether you would rely on that.

## Where this lives in the code

- `src/persistence/file_io.cpp` — `write_all`, `fsync_or_throw`,
  `fsync_directory`, `write_file_atomically`
- `docs/persistence.md`
- [../10-cpp-systems-foundations/06-system-calls.md](../10-cpp-systems-foundations/06-system-calls.md)

---

Next: [03-corruption.md](03-corruption.md)
