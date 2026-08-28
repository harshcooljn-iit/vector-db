# 00 — Start Here

Welcome. This repository is two things at once:

1. **A working vector database** written in C++20 — `vectordb`.
2. **A guided path** from "I'm strong at DSA and comfortable with C++" to
   "I can design and build a storage engine."

You are reading the entry point for (2).

---

## Who this is written for

The notes assume you already know, and will not re-explain:

arrays, `std::vector`, maps, sets, heaps, graphs, BFS/DFS, binary search,
sorting, big-O reasoning, classes, basic templates, and everyday Git.

They assume you have **not** yet worked with:

memory mapping, binary file formats, page faults, cache lines, `std::atomic`,
condition variables, thread pools, SIMD intrinsics, crash consistency, or the
internals of an approximate-nearest-neighbour index.

That gap — between *algorithms you can already implement* and *systems you have
never had to reason about* — is exactly what this project is built to close.

Wherever the two connect, the notes say so explicitly. HNSW is not presented as
an exotic new structure; it is presented as **a graph you already know how to
traverse, laid over data that does not fit in cache and does not fit in RAM.**

---

## What you will end up understanding

By the end you should be able to answer, without looking anything up:

- What actually happens to the 3 KB of floats when you insert one 768-dimensional vector.
- Why those floats are *not* stored in a `std::vector<std::vector<float>>`.
- Why a database refuses to `fwrite` a C++ struct straight to disk.
- Why searching 1,000,000 vectors can take under a millisecond, and what that costs you.
- What "recall@10 = 0.98" means, and who paid for the missing 0.02.
- Why one mutex around the whole database is a correct design and a bad one.
- What survives a `kill -9` mid-insert, and what you have to rebuild.

---

## The learning ladder

Each level assumes the one above it. Do not jump to HNSW on day one; it will
look like magic, and magic is not transferable knowledge.

```
Level 1   What you already have: C++ and DSA
              |
Level 2   C++ systems foundations      10-cpp-systems-foundations/
          ownership, memory layout, bytes on disk, syscalls
              |
Level 3   Build and dependencies       20-build-and-dependencies/
          CMake, vcpkg, Debug vs Release and why it changes your numbers
              |
Level 4   Vector search concepts       30-vector-database/
          what an embedding is, distance metrics, top-k
              |
Level 5   Indexes                      40-search-indexes/
          brute force first (the oracle), then ANN, then HNSW
              |
Level 6   Storage and persistence      50-storage/
          binary formats, mmap, SQLite, index files, atomic replacement
              |
Level 7   Concurrency                  60-concurrency/
          races, mutexes, shared_mutex, thread pools, atomics
              |
Level 8   Performance                  70-performance/
          cache locality, allocation, SIMD, profiling, honest benchmarking
              |
Level 9   Database engineering         80-database-engineering/
          consistency, crash safety, corruption, tombstones, rebuild
```

Difficulty labels used throughout:

- 🟢 **Foundational** — new vocabulary, but no new hard reasoning.
- 🟡 **Intermediate** — you will need to hold two ideas at once.
- 🔴 **Advanced** — expect to read it twice and run an experiment.

---

## How to use this repository

### 1. Build it first

Nothing here lands until you have run it.

```sh
export VCPKG_ROOT=/path/to/vcpkg          # where you cloned vcpkg
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/bin/vectordb version
```

If that fails, go to [90-debugging/01-how-to-debug-this-project.md](90-debugging/01-how-to-debug-this-project.md)
before going anywhere else.

### 2. Read a note, then read the code it points at

Every note ends with a **Where this lives in the code** section naming real
files and real functions. A note you read without opening the corresponding
`.cpp` is a note you will forget by Thursday.

### 3. Run the experiments

[100-exercises/](100-exercises/) contains hands-on changes with a stated goal,
a thing to measure, and questions that are deliberately not answered for you.
The answers are reachable by running the code. That is the point.

### 4. Read the Git history

The history is a designed artefact, not a byproduct. Each commit is one
coherent change with a message explaining *why*, not just what.

```sh
git log --oneline --reverse     # the project in the order it was built
git show <hash>                 # one decision, in full
git log --oneline -- src/index/hnsw/   # how HNSW evolved
```

Reading `git log --reverse` top to bottom is a legitimate way to learn this
codebase — arguably the best one.

---

## Map of the notes

| Directory | What it covers | Level |
|---|---|---|
| `10-cpp-systems-foundations/` | ownership, RAII, memory layout, bytes, syscalls | 🟢🟡 |
| `20-build-and-dependencies/` | CMake, vcpkg, build configurations | 🟢 |
| `30-vector-database/` | embeddings, distance metrics, top-k | 🟢🟡 |
| `40-search-indexes/` | brute force, ANN, HNSW intuition → algorithm → code | 🟡🔴 |
| `50-storage/` | binary formats, mmap, SQLite, index persistence | 🟡🔴 |
| `60-concurrency/` | threads, mutexes, condition variables, pools, atomics | 🟡🔴 |
| `70-performance/` | cache locality, allocation, SIMD, profiling, benchmarking | 🔴 |
| `80-database-engineering/` | consistency, crash safety, corruption, tombstones | 🔴 |
| `90-debugging/` | how to debug this project, common failure modes | 🟡 |
| `100-exercises/` | hands-on experiments and mini challenges | mixed |

Also at this level:

- [01-project-overview.md](01-project-overview.md) — what VectorDB is and is not
- [02-how-to-navigate-the-codebase.md](02-how-to-navigate-the-codebase.md) — "if you want X, open Y"
- [implementation-map.md](implementation-map.md) — concept → code → tests → docs
- [glossary.md](glossary.md) — every term, one line each
- [progress.md](progress.md) — what is actually built so far
- [final-capstone.md](final-capstone.md) — self-assessment, no answer key
- [next-steps.md](next-steps.md) — where to take this after v1

---

## A note on honesty

This project does not claim numbers it did not measure. Every figure in
`docs/benchmark-results.md` names the machine, the compiler, the build type and
the parameters that produced it. When something is unimplemented or partially
implemented, the docs say so.

You should hold your own work to the same standard. "It felt faster" is not a
result.

---

Next: [01-project-overview.md](01-project-overview.md)
