# HNSW — the algorithm

🔴 Advanced · read [03-hnsw-intuition](03-hnsw-intuition.md) first

The intuition note said *what* HNSW does. This one is precise about *how*, in
enough detail that you could implement it.

## Notation

- `q` — the query vector
- `ef` — candidate-list width; how many near candidates we keep while searching
- `M` — neighbour budget per node above layer 0; layer 0 gets `2M`
- `L` — a node's top layer
- `mL = 1 / ln(M)` — the level-generation constant

## Level assignment

```
level = floor( -ln(U) × mL )        where U is uniform in (0, 1]
```

An exponential distribution. `P(level ≥ ℓ) = M^-ℓ`, so each layer holds about
`1/M` of the layer below.

Why that specific constant? Because you want the expected number of layers to be
`log_M(N)` — the same shape as the search cost. Larger `mL` makes a taller tower
with more descent steps; smaller makes a flatter one that degenerates toward a
single graph. `1/ln(M)` is the value that balances the two, and it is the
parameter that makes HNSW sub-linear rather than merely clever.

```cpp
float u = rng_.uniform();
if (u <= 0.0F) u = std::numeric_limits<float>::min();   // ln(0) is -inf
const double level = -std::log(double(u)) * config_.level_multiplier();
return std::size_t(std::min(level, 254.0));             // u8 storage cap
```

The clamp is not paranoia: an unlucky draw of `1e-30` yields level 24 at M=16,
and while `u8` tolerates that, an unbounded value would eventually overflow the
storage and corrupt the graph.

## `search_layer(q, entry_points, ef, layer)`

The core routine. Everything else calls it.

```
visited ← entry_points
frontier ← min-heap by distance   (which node to expand next)
best     ← max-heap by distance   (the ef closest found; root is the WORST)

for e in entry_points:
    d ← distance(q, e); push e into frontier and best

while frontier is not empty:
    c ← pop closest from frontier
    if |best| = ef and c is worse than best.root:
        break
    for each neighbour e of c on this layer:
        if e already visited: continue
        mark e visited
        d ← distance(q, e)
        if |best| < ef or d < best.root.distance:
            push e into frontier
            push e into best
            if |best| > ef: pop best.root
return best, sorted ascending
```

### Why two heaps, in opposite directions

They answer different questions, and each wants a different element at its root:

- **frontier**: "what should I expand next?" → the *closest* unexpanded node.
- **best**: "is this candidate good enough to keep?" → compare against the
  *worst* result currently held, because that is the one being evicted.

This is the same max-heap-for-k-smallest inversion as
[top-k](../30-vector-database/04-top-k-search.md), and it appears here for the
same reason: **a bounded collection is ordered by its eviction criterion, not
its goal criterion.**

In the code both are `std::vector<Candidate>` with `push_heap`/`pop_heap`, using
two comparators that are exact inverses:

```cpp
constexpr CandidateBetter kBetter{};   // best:     root is the worst kept
constexpr CandidateWorse  kWorse{};    // frontier: root is the closest
```

### The termination condition — where the approximation lives

```cpp
if (best.size() >= ef && kBetter(best.front(), current)) break;
```

"The nearest thing left to explore is worse than the worst result I already
hold, so stop."

This is a **heuristic, not a proof**. A closer node could be reachable through a
worse one — nothing forbids it. The bet is that in a small-world graph, closer
nodes are usually reachable through closer nodes.

That single line is where exactness is traded for speed. Everything else in HNSW
is exact given the graph; this is the approximation. Worth sitting with.

### Visited tracking

The obvious approach is a `std::vector<bool>` cleared per query. At a million
nodes that is a 125 KB memset before every search — frequently more work than
the search, which may touch a few thousand nodes.

Instead, each slot holds the *generation* that last visited it:

```cpp
void begin_search() { if (++generation_ == 0) { fill(stamps_, 0); generation_ = 1; } }
bool visit(LocalId id) {
    if (stamps_[id] == generation_) return false;
    stamps_[id] = generation_;
    return true;
}
```

Reset is `O(1)`. The full clear happens on the four-billionth query, when the
generation wraps — and the wrap is handled, because "it will never happen" is
how you get a bug in year three.

**Version stamps instead of clearing** is a broadly reusable trick: any time a
large scratch structure is reused across many small operations, ask whether you
can invalidate instead of erase.

## `search(q, k, efSearch)`

```
ef ← max(k, efSearch)                    # or the answer is truncated silently
ep ← entry_point
for layer from max_level down to 1:
    ep ← greedy_descend(q, ep, layer)    # width 1
W ← search_layer(q, {ep}, ef, 0)         # width ef
return the first k of W that are not tombstoned
```

**Why width 1 above layer 0?** Those layers are sparse and exist only to reach
the right neighbourhood. A wide search there would compute many distances to
buy precision that phase 2 discards immediately.

**Why `ef ← max(k, ef)`?** If a caller asks for `k = 100` with `efSearch = 64`,
the candidate list can only ever hold 64, so 36 results would be missing with
nothing reporting it.

**Why filter tombstones after, not during?** A deleted node still has useful
edges and still routes traffic. Skipping it during traversal would cut those
edges and can disconnect the region it was bridging.

## `insert(v)`

```
L ← random_level()
allocate link storage for layers 0..L
if the graph is empty: entry_point ← v; max_level ← L; return

ep ← entry_point
for layer from max_level down to L+1:
    ep ← greedy_descend(v, ep, layer)          # layers v does not occupy

entry_points ← {ep}
for layer from min(L, max_level) down to 0:
    W ← search_layer(v, entry_points, efConstruction, layer)
    neighbours ← select_neighbours(W, max_neighbours(layer))
    set v's links on this layer to neighbours
    for each n in neighbours:
        link(n, v, layer)                      # the reverse edge
    entry_points ← W                           # all of them, for the next layer

if L > max_level: max_level ← L; entry_point ← v
```

Three details that are easy to get wrong:

**The reverse edge.** Without `link(n, v, layer)` the new node points outward but
nothing points back, so nothing can reach it. Recall for recently-inserted
vectors collapses, and nothing errors.

**`entry_points ← W`, not just the closest.** Descending from all `ef` candidates
found on the layer above costs almost nothing — they are already visited — and
makes the next layer's search far less likely to start in the wrong basin.

**Insertion is a search.** Building N vectors is roughly N searches, hence
`O(N log N)`. This is also why `efConstruction` costs build time and nothing at
query time: it is the cheapest quality knob if you build once and query often.

## `select_neighbours(candidates, M)` — the heuristic

Algorithm 4 in the paper, and the part most often simplified away.

```
sort candidates by distance to the target, ascending
result ← []
for e in candidates:
    if |result| = M: break
    keep ← true
    for r in result:
        if distance(e, r) < distance(e, target):
            keep ← false                # e is reachable through r
            break
    if keep: result.append(e)

if |result| < M: fill from the rejects, in distance order
```

### Why not simply take the M closest?

Picture a node at the edge of a dense cluster. Its M nearest neighbours are all
*inside* the cluster — all in one direction. The node becomes a dead end: a
search arriving from outside can enter but has no edge pointing anywhere useful,
and a search needing to cross to the other side has no edge to take.

The heuristic's condition — "accept `e` only if it is closer to me than to
anything I have already picked" — has a clean geometric reading. If `r` is
already chosen and `distance(e, r) < distance(e, target)`, then `e` sits in
`r`'s "shadow": whatever direction `e` covers, `r` covers already. Spend the
edge elsewhere.

The result is a neighbourhood fanning out in different directions. Greedy search
only works if *some* edge points roughly toward the query, wherever it is.

### Why fill from the rejects

If the heuristic is strict enough to leave the list short, we top it up from the
rejected candidates in distance order. An under-filled neighbour list wastes
memory that is already paid for (the slot exists in the flat array either way),
and a sparser graph is a slower one.

### Re-pruning on a full list

When a reverse edge arrives at a node whose list is full, we do **not** simply
drop it or evict the furthest. We re-run the heuristic over the existing
neighbours plus the newcomer.

- Dropping the new edge lets early insertions monopolise a node's budget
  forever, so the graph ossifies around whatever was inserted first.
- Evicting the furthest discards exactly the long-range diversity the heuristic
  exists to protect.

## Complexity

| Operation | Cost |
|---|---|
| search | `O(ef × M × D × log N)` distance computations, roughly |
| insert | one search, plus `O(M)` re-prunes |
| build | `O(N log N)` searches |
| memory | `O(N × M)` edges |

The `log N` comes from the layer count, `ef × M` from the width of the layer-0
exploration, and `D` from each distance.

The honest caveat: these are *expected* costs under the assumption that the
graph has small-world structure. There is no worst-case guarantee. A
pathological dataset — uniform noise in high dimensions — degrades toward
linear, which is precisely the case the
[ANN note](02-approximate-nearest-neighbour.md) warns about.

## Experiments

1. Instrument `search_layer` to count distance computations. Compare with `N`
   at various `ef`. That ratio is what the graph bought you.
2. Delete the `break` in the termination check and re-measure. You now have an
   exhaustive search of the connected component — compare recall and time.
3. Replace `select_neighbours` with "take the closest M". Re-run the recall
   suite. The most instructive one-line change in the codebase.
4. Remove the reverse edge (`link(n, v, layer)`) and measure recall separately
   for the first and last 10% of inserted vectors. Watch the second group
   collapse.
5. Set `mL` to 1.0 and to 0.1. Count layers and measure. You are exploring what
   `1/ln(M)` is balancing.

## Where this lives in the code

- `src/index/hnsw/hnsw_index.cpp` — `random_level`, `greedy_descend`,
  `search_layer`, `select_neighbours`, `link`, `add`, `search`, and `VisitedSet`
- `include/vectordb/index/hnsw_config.hpp` — the parameters and `level_multiplier`
- `docs/hnsw.md` — the specification, with the measured recall table

---

Next: [05-hnsw-implementation.md](05-hnsw-implementation.md)
