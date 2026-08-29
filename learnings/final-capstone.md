# Final capstone

A self-assessment. **No answer key** — every answer is in the code, the tests, or
the commit history, and finding it is the exercise.

Work through it with the repository closed. Where you cannot answer, note it,
then go and look. What you had to look up is the map of what you actually
learned.

---

## Part 1 — The life of a vector

You run:

```sh
vectordb insert papers.vdb --vector "0.1,0.2,..." --id 42 --meta year=2026
```

1. Trace the call from `main` to the bytes on disk. Name every layer it passes
   through.
2. What is validated, in what order, and where? What happens to the store if
   validation fails halfway?
3. Where do the floats end up in memory? At what address, relative to the vector
   inserted before it?
4. What is computed and cached at insert time that is *not* the vector itself?
   Why then rather than at query time?
5. `LocalId` and `VectorId` are both assigned. Which one does the HNSW graph
   store, and what would it cost to store the other?
6. When does `year=2026` reach disk? When does the vector? Why are they
   different?
7. Kill the process immediately after the command returns. What survives?

---

## Part 2 — The life of a query

```sh
vectordb search papers.vdb --query "0.1,0.2,..." --k 10 --ef-search 64
```

8. Describe the two phases of HNSW search. Why is the first narrow and the second
   wide?
9. `search_layer` maintains two heaps in opposite directions. What question does
   each answer, and what is at each root?
10. Write down the line where the *approximation* lives — the one place where the
    algorithm gives up a guarantee. Why is it a heuristic and not a proof?
11. At `k = 10` with `efSearch = 64`, roughly how many distance computations
    happen? Compare with brute force at N = 50,000.
12. The results come back as `LocalId` and `RankKey`. Where are they converted,
    and why not earlier?
13. Add `--filter 'year >= 2020'`. What changes? Under what conditions does this
    return fewer than 10 results even though 10 matching vectors exist?
14. What does `VisitedSet` do between queries, and why is it not a `memset`?

---

## Part 3 — Storage

15. Why is a C++ struct never written directly to a file? Give two independent
    reasons.
16. `vectors.bin` stores four separate arrays rather than interleaved records.
    Give the two reasons, and say which is decisive.
17. A length is read from a file and used to size a `std::vector`. What is the
    rule, and what happens without it?
18. `slot_count * dimension * 4` is computed from file values. What is checked
    before that multiplication, and what would a corrupt header otherwise
    produce?
19. Describe the four steps of an atomic file write. Which is most often
    forgotten, and what breaks?
20. On macOS, `fsync` is not enough. What is, and what does it cost?
21. What is *not* validated on open, and why is that the right call? What is the
    cheapest thing that would close the gap?

---

## Part 4 — Deletion and recovery

22. What does deleting a vector actually do — to the store, to the graph, to the
    metadata?
23. Why does the HNSW node stay in the graph? Give the failure mode that removing
    it risks.
24. Tombstones are filtered *after* traversal, not during. What would break if
    that changed?
25. `compact` renumbers `LocalId`s. Why does that not break any user reference?
26. The index file is corrupt. What happens on open, and why is that not a
    failure?
27. Which file, if lost, means data loss? Why are the other two different?

---

## Part 5 — Concurrency

28. State the contract: what may run simultaneously, and what may not?
29. Why a `shared_mutex` rather than a `mutex`? Rather than lock-free?
30. `MetadataStore` breaks the project's locking rule. Why must it?
31. `HnswIndex::search` is `const` and uses `thread_local` scratch. Why can it not
    be a member? What does `thread_local` cost?
32. `batch_search` writes `results[i]` from many threads with no synchronisation.
    Why is that not a data race? Name the type for which it *would* be.
33. Why does `batch_search` take the lock per query instead of once?
34. What would a stress test miss that ThreadSanitizer catches?

---

## Part 6 — Performance

35. Brute force at D = 768, N = 100,000 achieves ~48 GB/s regardless of
    dimension. What is that number, and what does it tell you the loop is
    limited by?
36. NEON is 3.9× faster than scalar on the kernel and roughly nothing on the
    scan. Explain, in one sentence.
37. Both the scalar and SIMD kernels use four accumulators. Why? What are the two
    separate benefits?
38. Why is `-ffast-math` not enabled? Give both reasons.
39. HNSW's index costs 145 bytes per vector, independent of dimension. Why
    independent? At what dimension does it stop mattering?
40. The batch benchmark once reported 13 million queries/second. What was wrong,
    and what rule prevents it now?
41. A search scaling collapse was traced to a bottleneck two layers away.
    Describe the method that found it, and the four hypotheses that did not.

---

## Part 7 — Judgement

Questions with no single right answer. Argue your position.

42. `M = 16` or `M = 32` for 10M documents at D = 768 on a 64 GB machine? Show
    the arithmetic.
43. Would you enable automatic compaction above 50% tombstones? What is the
    failure mode?
44. Is the missing-key-never-matches rule right? Argue the other side.
45. Should the filter language have `or`? What would it cost, concretely?
46. Payload checksums are not verified on open. Defend the decision, then attack
    it.
47. The AVX2 kernel has never run on x86 hardware. Is shipping it responsible?
48. Where would you spend the next week: PQ, mmap-without-loading, WAL, or an
    hnswlib baseline? Justify against a use case.

---

## Part 8 — Build it yourself

The real test. Without looking at this repository:

49. Sketch the file format for a new index type. Include everything needed to
    validate it.
50. Design the concurrency model for a system with 10 writers and 1000 readers.
    What changes from ours, and why?
51. You are told recall dropped from 0.99 to 0.94 after a commit. List, in order,
    what you would check.
52. Design a benchmark for filtered search. What must it control for? What single
    number, if any, could summarise it?

---

## Scoring yourself

- **Parts 1–2**: if you cannot trace insert and search end to end, re-read
  [`02-how-to-navigate-the-codebase.md`](02-how-to-navigate-the-codebase.md) and
  follow the code.
- **Parts 3–4**: gaps here mean the storage notes deserve another pass — this is
  the material furthest from typical application programming.
- **Part 5**: if 30, 32 or 34 were hard, that is the most valuable material in
  the repository. Read [60-concurrency/05](60-concurrency/05-reader-writer-concurrency.md)
  and run the `tsan` preset.
- **Part 6**: if 35 and 36 were hard, do [Exercise 6](100-exercises/06-simd-vs-scalar.md).
  The distinction between compute-bound and memory-bound is the single most
  transferable idea here.
- **Part 7**: no wrong answers, only unargued ones.
- **Part 8**: if you can do these, you did not just read a codebase — you learned
  to build one.
