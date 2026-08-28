# Top-k — keeping the best 10 of 5,000,000

🟡 Intermediate · pairs with `src/core/top_k.cpp`

You know heaps. This note is about *which* heap, in *which* direction, and why
the answer is the opposite of most people's first instinct.

## The problem

A brute-force search computes N distances and must return the k smallest.
N = 5,000,000, k = 10.

## Approach 1: sort everything

```cpp
std::vector<Candidate> all;               // 5,000,000 entries
for (id : everything) all.push_back({id, distance(query, store[id])});
std::sort(all.begin(), all.end());
all.resize(10);
```

Correct, and bad in two independent ways:

- **Time**: `O(N log N)`. About 5M × 22 comparisons.
- **Memory**: `O(N)`. 5,000,000 × 8 bytes = 40 MB, allocated and written per
  query.

The memory is the worse problem, and not for the obvious reason. Those 40 MB
are *written* while the scan is also *reading* 15 GB of vectors. They evict the
vector data you are streaming, and they double the memory traffic of a loop that
was already bandwidth-bound.

## Approach 2: a bounded heap

Keep only the best k seen so far.

```cpp
TopKCollector collector(10);
for (id : everything) collector.offer(id, distance(query, store[id]));
auto results = collector.take_sorted();
```

- **Time**: `O(N log k)` worst case — but see below, the real case is better.
- **Memory**: `O(k)`. **Ten candidates. 80 bytes.**

80 bytes stays in L1 cache forever. The scan streams past it and never disturbs
it. That is the actual win: not the `log k` versus `log N`, but the fact that
the bookkeeping structure disappears into cache.

## The direction that trips everyone up

We want the k **smallest** keys. So — a min-heap?

**No. A max-heap.**

Ask what question the loop asks per candidate. It is not "what is the best thing
I have?" It is:

> "Is this candidate better than the **worst** thing I am currently keeping?"

Because that is the one that gets evicted. So the element that must be reachable
in `O(1)` is the *worst* one, and a max-heap (by rank key, where larger = worse)
puts exactly that at the root.

```
max-heap of the current best 4, by rank key:

              [9.1]  <- root: the WORST kept. The only one we compare against.
             /     \
         [4.2]     [7.8]
         /
     [1.5]         <- the actual best, buried. We do not need it until the end.
```

The best candidate is buried at a leaf, and that is fine — nobody asks for it
until `take_sorted()` at the very end.

**This inversion is a general pattern.** Whenever you keep a bounded best-N
collection, the heap orders by the *eviction* criterion, not the *goal*
criterion. Same for "k largest" (use a min-heap), same for LRU caches, same for
bounded priority queues in A*.

## Why it is nearly O(N) in practice

The worst case is `O(N log k)` — every candidate an improvement. That requires
the input to arrive in decreasing-distance order, which real data does not do.

In practice, after the first k candidates the heap is full and most candidates
lose a **single comparison** against the root:

```cpp
if (!better(candidate, heap_.front())) return;    // O(1) — the common path
```

As the scan proceeds the threshold tightens, so the acceptance rate falls
roughly like `k/i` at candidate `i`. Summed over N, the expected number of
accepted candidates is about `k · ln(N/k)` — for N = 5M, k = 10, roughly **130
insertions** out of five million offers. The other 4,999,870 cost one float
comparison each.

So the heap is effectively `O(N)` with a tiny constant, plus `O(k log k)` once
at the end.

## Determinism, and why it is not a cosmetic concern

What happens when two candidates have the *same* distance?

Without a rule, the answer is "whichever the heap happened to keep" — which
depends on offer order, which depends on which thread got which chunk. Two runs
over identical data return different, equally-good result sets.

That sounds harmless until you measure recall:

```
recall@10 = |HNSW results ∩ brute-force results| / 10
```

If both sides are non-deterministic at ties, this number wobbles for reasons
that have nothing to do with HNSW. You would be measuring heap noise and
attributing it to the index.

So `CandidateBetter` defines a **total order**: key first, then `local_id`
ascending.

```cpp
if (a.key != b.key) return a.key < b.key;
return a.local_id < b.local_id;
```

Now the k-best set is a property of the data alone. There is a test
(`ResultIsIndependentOfOfferOrder`) that shuffles the input twenty times and
demands an identical answer.

## The `would_accept` subtlety

`would_accept(key)` lets a caller skip expensive work — a metadata lookup, a
filter evaluation — for a candidate that cannot make the cut.

It takes only a key. But acceptance is decided by `(key, local_id)`. A candidate
tying the current worst on key can still win on `local_id`.

So `would_accept` uses `<=`, not `<`. It is **conservative**: it may say yes to
something `offer` then rejects, but it must never say no to something `offer`
would keep. Get this backwards and two callers — one using the pre-filter, one
not — silently return different results.

This is worth staring at, because it is the shape of a whole class of bug:
**when a fast pre-filter approximates a slower exact test, it must err in the
direction of doing more work, never less.**

## `take_sorted`: the one place we sort

```cpp
std::sort_heap(heap_.begin(), heap_.end(), kBetter);
```

`std::sort_heap` on a max-heap yields ascending order — best first. `O(k log k)`
with k = 10 is about 33 comparisons, once per query. Compare that with sorting 5
million.

The general principle again: **do the expensive-per-item thing to k items, not
to N.** The same reasoning puts `score_from_rank_key` at the API boundary and
the `LocalId → VectorId` translation after the top-k rather than inside it.

## Merging, for parallel search

`merge_top_k` combines per-thread results. Each thread produces its own sorted
top-k; the merge re-offers them all into one collector.

A proper k-way merge with a heap of cursors would be asymptotically better. It is
not used, because the number of lists is the *thread count* — single digits —
and each holds at most k. `O(T·k log k)` with T = 8 and k = 10 is a few hundred
operations, against considerably more code to get wrong.

One optimisation is kept, and it is free: each list is sorted best-first, so
once a candidate is rejected, every later candidate in that list is worse and
can be skipped with a `break`. This is exactly where the conservative
`would_accept` earns its keep — a strict `<` would break early on a tie and
silently drop a candidate that should have won on `local_id`.

## "Why not just..."

**"...`std::nth_element`?"** `O(N)` and gives exactly the k best. But it needs
all N candidates materialised first — the 40 MB problem again. It is the right
tool when you already have the array; the wrong one when you are streaming.

**"...`std::priority_queue`?"** That *is* a heap, and it would work. We use a
raw `std::vector` plus the heap algorithms because `priority_queue` offers no
way to replace the top element in place — you must `pop()` then `push()`, which
sifts twice. And it hides the container, so `reserve()`ing once up front is
awkward. Fine for k=10; the direct form is clearer about the cost.

**"...keep a sorted array and binary-search the insertion point?"** Binary
search is `O(log k)`, same as the heap. But insertion then *shifts* up to k
elements. For k = 10 the shift is a memmove of 80 bytes and may genuinely win on
cache-friendliness; for k = 1000 it is much worse. The heap has no such cliff.

## Experiments

1. Implement approach 1 (sort everything) and approach 2, and time both over
   1,000,000 candidates at k = 10 in `release`. Then repeat at k = 1000, and at
   k = 100,000. Find where the crossover is, and explain it.
2. Count the accepted offers in a scan of 1,000,000 random candidates at k = 10.
   Compare with the `k · ln(N/k)` estimate (~115). Then feed the same
   candidates sorted worst-first and count again — you have just constructed
   the worst case.
3. Remove the `local_id` tie-break from `CandidateBetter` and run
   `ResultIsIndependentOfOfferOrder`. Then imagine that failure appearing as a
   0.02 recall wobble six weeks later, with no test pointing at it.
4. Change `would_accept` from `<=` to `<` and run `MatchesASingleCollectorOver-
   TheSameCandidates`. Note that it fails only when ties happen to straddle the
   boundary — a bug that would pass CI most days.
5. Replace the max-heap with a min-heap and try to keep the k smallest. Work out
   what you now have to do per candidate, and how much it costs.

## Where this lives in the code

- `include/vectordb/core/top_k.hpp` — the collector, with the max-heap reasoning
- `src/core/top_k.cpp` — `offer`, the single hot comparison, `merge_top_k`
- `include/vectordb/core/search_result.hpp` — `Candidate`, `CandidateBetter`
  (the total order), `SearchResult`, and the boundary translation
- `tests/unit/core_top_k_test.cpp` — order-independence, tie handling, and a
  cross-check against a full sort on 5,000 random candidates

---

Next: [../40-search-indexes/01-brute-force-index.md](../40-search-indexes/01-brute-force-index.md)
