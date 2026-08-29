# HNSW — the implementation

🔴 Advanced · read with `src/index/hnsw/hnsw_index.cpp` open

The algorithm note said what to compute. This one is about the decisions that
only appear once you write it down: where the bytes go, what allocates, and what
is safe to do concurrently.

## Where the memory goes

Two layers with completely different shapes, so two different layouts.

### Layer 0: a flat array with a fixed stride

Every node is here, and this layer takes essentially all of the search traffic.

```
layer0_links_:  [cnt][n0][n1]...[n31] [cnt][n0][n1]...[n31] ...
                |<--- 2M + 1 = 33 --->|
                node 0                 node 1
```

```cpp
const LocalId* links(LocalId node, 0) {
    return layer0_links_.data() + node * layer0_stride();
}
```

One multiply. No indirection, no allocation per node, no pointer to chase.

**The count lives in slot 0**, not in a parallel array. On a 64-byte cache line
that is 16 `LocalId`s, so reading "how many neighbours" pulls in the first 15 of
them for free — and you were about to read them anyway. A separate count array
would cost a second cache miss on every single node visit, in the innermost loop
of the system.

This is also where [ADR-0003](../../docs/decisions/ADR-0003-id-model.md) pays
off. At N = 1M and M = 16 this array is `1M × 33 × 4` = **132 MB** with a 32-bit
`LocalId`, and 264 MB with a 64-bit one. Halving it halves the cache pressure of
the pointer-chasing phase, which is the part of HNSW that is *not* arithmetic.

### Upper layers: one shared arena

Only about `1/M` of nodes reach layer 1, `1/M²` reach layer 2. A fixed stride
across all nodes would be ~94% empty.

```
upper_offsets_: [kNone][ 0 ][kNone][ 34 ]...      one u32 per node
upper_links_:   [cnt][n0]..[n15][cnt][n0]..       append-only arena
                |<-- M + 1 = 17 -->|
```

Append-only, because a node's level is fixed at insertion and never changes.
Nothing is ever freed or moved, so an offset stays valid forever.

```cpp
const LocalId* links(LocalId node, std::size_t layer) {          // layer >= 1
    return upper_links_.data() + upper_offsets_[node] + (layer - 1) * upper_stride();
}
```

**A hazard worth naming.** `upper_offsets_[node]` is `kNoUpperLinks`
(`0xFFFFFFFF`) for a node with no upper layers, and that arithmetic would land
wildly out of bounds. It is safe only because every caller goes through
`neighbours()`, which checks `layer > node_level_[node]` *first*. That is an
invariant enforced by discipline, not by the type system — which is exactly the
kind of thing worth a comment and a test.

### What we did not do

`std::vector<std::vector<LocalId>>` per node would be simpler. It is the same
mistake as `std::vector<std::vector<float>>` for the vectors: a million
allocations, ~40 MB of pointer overhead, and a traversal that pointer-chases
through scattered heap blocks. The graph walk is *already* random-access; adding
a second level of indirection to it is the worst place to do so.

## The scratch problem

`search_layer` needs a visited set (`O(N)`), two heaps, and some working
vectors. Allocating those per query would dominate a search that touches a few
thousand nodes.

But `search()` is `const` and must be safe under concurrent readers, so the
scratch cannot be a member — that would be a data race the moment two threads
search at once.

```cpp
SearchScratch& scratch_for_this_thread() {
    thread_local SearchScratch scratch;
    return scratch;
}
```

Each thread gets its own. No locking, no per-query allocation, and it grows to
the largest index that thread has searched.

**The costs, stated rather than hidden:**

- The buffers live as long as the thread. A thread that searched a 10M-node
  index keeps a 40 MB visited set even if it never searches again.
- A thread searching two differently-sized indexes keeps the larger.
- `thread_local` access is slightly more expensive than a member — a TLS lookup
  rather than an offset from `this`. Once per `search_layer` call, so it is
  noise.

The alternative — a caller-supplied context object — is more flexible and
uglier at every call site. For a search path this is the right trade, and the
header says so.

## Iterative, never recursive

```cpp
bool improved = true;
while (improved) { ... }        // greedy_descend
while (!frontier.empty()) { ... }  // search_layer
```

Graph traversal has no bounded depth. A recursive `search_layer` would blow the
stack on a large index, and the failure would be a crash with no useful message
rather than an exception — the kind of bug that only shows up in production
because your test data was smaller.

## `add` must be called in slot order

```cpp
if (local_id != node_level_.size())
    throw InvalidArgumentError("nodes must be added in ascending slot order");
```

This lets `node_level_`, `layer0_links_` and `upper_offsets_` all be plain
`push_back`/`resize` operations indexed directly by `LocalId`, with no map from
id to graph position.

It is a real constraint on callers, so it is checked and reported rather than
assumed. The database layer satisfies it naturally: the store assigns slots
densely and ascending, and a rebuild replays them in the same order.

## Determinism, and why it is load-bearing

Two things could make a build non-reproducible: the level RNG, and insertion
order. Both are pinned — the RNG is seeded from the config, and insertion order
is the store's slot order.

So two indexes built from the same data, config and seed have **byte-identical
graphs**, and there is a test that walks every neighbour list of both to prove
it.

This is not tidiness. Recall is a noisy measurement; if the graph also varied
run to run, a recall change could never be attributed to a code change. Pinning
the build is what makes `recall@10 dropped from 0.99 to 0.94` actionable.

## Reading the code

Roughly in dependency order:

| Function | Lines | What to notice |
|---|---|---|
| `VisitedSet` | ~30 | generation stamps; the wrap case is handled |
| `random_level` | ~10 | the `u <= 0` guard, and the clamp to 254 |
| `links` / `neighbours` | ~20 | the two layouts; the `layer > level` guard |
| `greedy_descend` | ~20 | width-1 walk, iterative |
| `search_layer` | ~60 | two heaps in opposite directions; the early exit |
| `select_neighbours` | ~50 | the diversity heuristic, then the top-up |
| `link` | ~35 | re-prune rather than drop or evict-furthest |
| `add` | ~70 | allocate, descend, connect both ways, maybe re-crown |
| `search` | ~45 | descend, explore, filter tombstones |

## A code-review checklist for a graph index

What an experienced reviewer would actually look for here. Try each against the
code before reading the answer.

**Are node ids stable?** — Yes: `LocalId` is assigned by the store and never
reused while the node exists. If ids could shift, every neighbour list would
need rewriting.

**Can a neighbour reference dangle?** — Every id written comes from
`search_layer`, which only ever yields existing nodes. On load, references are
validated against the node count *before* the graph is used, so the search loop
never bounds-checks. There is a test asserting no dangling, self- or duplicate
references.

**Is the visited set correctly sized?** — `resize` is called before every search
with the current node count. If a node were added between the resize and the
walk, `visit()` would read out of bounds — which is one more reason `add` and
`search` are documented as mutually exclusive.

**Are distances computed with the right norms?** — `with_norms` reads the
*cached* norm. If the store were mutated without refreshing the accessor, cosine
would use a stale norm and silently return wrong distances. Hence
`set_accessor`.

**Can the graph become disconnected?** — Yes, in principle, if a bridging node
is removed. That is exactly why deletion is a tombstone rather than a removal.

**Is the entry point always valid?** — It is `kInvalidLocalId` only while the
graph is empty, and `search` returns early in that case. It is only ever
reassigned to a node that has just been fully linked.

**Do serialized offsets get validated?** — Yes, and that is the subject of
`hnsw_file.cpp`. Never trust an offset from a file.

## Experiments

1. Change `layer0_stride()` to store the count in a separate array and measure.
   You are isolating one cache line per node visit.
2. Make `SearchScratch` a `mutable` member instead of `thread_local`, then run
   the concurrency tests under the `tsan` preset. Read the race report.
3. Replace the flat `layer0_links_` with `std::vector<std::vector<LocalId>>` and
   compare build time, search time and RSS at N = 100,000.
4. Remove the `layer > node_level_[node]` guard in `neighbours()` and run under
   `asan`. This is the out-of-bounds read that the guard is silently preventing.
5. Print `index_bytes() / node_count()` for M = 4, 16, 64 and compare against
   the formula in `docs/hnsw.md`.

## Where this lives in the code

- `src/index/hnsw/hnsw_index.cpp` — everything above
- `include/vectordb/index/hnsw_index.hpp` — the layout, documented on the class
- `tests/unit/index_hnsw_test.cpp` — structural invariants and recall
- `docs/hnsw.md` — the specification and the memory formula

---

Next: [../50-storage/05-index-persistence.md](../50-storage/05-index-persistence.md)
