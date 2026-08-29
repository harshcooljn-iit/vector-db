# HNSW — the intuition

🟡 Intermediate · read this before the algorithm

No formulas here. The goal is that by the end, HNSW feels like something you
could have invented.

## Start with something you know: greedy graph search

Build a graph over your vectors where each node is connected to some of its
near neighbours. To find the nearest neighbour of a query:

```
current = any node
loop:
    look at current's neighbours
    if one is closer to the query than current, move there
    otherwise stop
```

That is hill climbing. You already know it.

```
        (start)
           ●
          /
         ● ────► ● ────► ●  ← closest reachable
                          \
                           ○ query
```

It works, and it is fast: each step examines only `M` neighbours, and each step
gets closer.

## The problem: local minima

```
                              ○ query
                             /
   ● ── ● ── ●              /
        │    │             /
        ●────●  ← stuck here: every neighbour is further away
                    ...but the true answer is over there ──►  ●
```

Greedy search stops at the first node where no neighbour improves. In a graph of
only *near* neighbours, that happens constantly — you get trapped in whatever
cluster you started in, because there are no edges out of it.

**Fix attempt 1: add long edges.** Now you can escape a cluster. But long edges
are useless when you are close to the answer, and every node has a fixed
neighbour budget — spend it on long edges and you have fewer short ones for the
final approach.

**Fix attempt 2: start from many random points.** Helps, and multiplies the
work by the number of starts.

Neither is satisfying. The real insight is that you want long edges *early* and
short edges *late* — which is a question of **when**, not of which edges exist.

## The insight: a skip list, for space

Recall a skip list. A sorted linked list is `O(n)` to search. Stack sparse
express lanes on top of it, each holding ~1/2 of the level below:

```
L2:  1 ──────────────────────────► 9
L1:  1 ──────► 4 ──────► 7 ──────► 9
L0:  1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9
```

Search the top lane in big strides, drop down when you overshoot, refine.
`O(log n)`.

**HNSW is a skip list for metric space.**

```
Layer 2:   ●                    ●                       ← ~1/M² of nodes
            \                  /                          huge strides
             \                /
Layer 1:   ●   ●        ●    ●      ●                   ← ~1/M of nodes
            \ / \      / \  /      /                      medium strides
             ●   ●    ●   ●●      ●
Layer 0:   ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ●      ← every node
                                                          fine detail
```

Each node is assigned a random top layer, geometrically: about `1/M` of nodes
reach layer 1, `1/M²` reach layer 2, and so on. A node present at layer `L`
appears in every layer below it too.

Because the top layer is nearly empty, its nodes are spread far apart, so its
edges connect distant regions. **The long edges are not designed — they are a
consequence of sparseness.** That is the elegant part.

## Searching

Two phases.

**Phase 1 — descend.** From the single entry point at the top layer, greedily
walk to the closest node, then drop to the next layer and continue from there.
Width 1: keep only the best node at each step.

These layers are sparse, so each step covers enormous distance. They exist only
to get you into the right neighbourhood cheaply, and spending a wide search on
them would buy precision you are about to discard.

**Phase 2 — explore layer 0.** Now do a *wide* best-first search, keeping the
best `efSearch` candidates rather than just one.

Why wide only here? Because layer 0 has all the detail and this is where the
answer actually is. `efSearch` is the dial: wider means more of the local
neighbourhood examined, higher recall, more time.

```
                 ○ query
   Layer 2:   ● ─────────────────► ●         two hops, crosses the whole space
                                    │ drop
   Layer 1:                    ● ──►● ──►●   a few hops, right region
                                         │ drop
   Layer 0:                      ● ● ● ● ●   wide search, exact neighbourhood
                                  ↑ ↑ ↑
                                  results
```

The local-minimum problem is solved not by escaping one, but by **arriving
already inside the right basin**.

## Inserting

Almost the same procedure, which is why the implementation shares code:

1. Roll the new node's top layer from the geometric distribution.
2. Descend greedily from the top, exactly as a search would, down to that layer.
3. From there down to layer 0: run a *wide* search (`efConstruction`) to find
   candidate neighbours, pick some, and connect **both ways**.

The reverse edge matters enormously. Without it the new node points at its
neighbours but nothing points back, so nothing can ever reach it, and recall for
recently-inserted vectors silently collapses.

Note that inserting a node is roughly as expensive as searching for it. Building
an index of N vectors costs about N searches — that is why build time is
`O(N log N)` and why `efConstruction` is a build-time-only cost.

## Choosing neighbours: the part that is not obvious

The new node found 200 candidates and may keep 16. Take the 16 closest?

**No.** And this is the single most important detail in HNSW.

```
Taking the 16 closest:                 The heuristic:

        ●●●●                                ●
       ●●●●●                              ●   ●
      ●●[N]        ← dense cluster           [N]
                     to the left           ●     ●
   all 16 edges point left                   ●  ●
   nothing points right                edges fan out in all directions
```

If the new node sits at the edge of a dense cluster, its 16 nearest neighbours
are all *inside* that cluster — all in the same direction. The node becomes a
dead end: a search arriving from elsewhere can enter but cannot usefully
continue, and a search needing to cross to the other side has no edge to take.

The heuristic instead asks, for each candidate `e` in increasing distance
order:

> Is `e` closer to me than to anything I have already picked?

If some already-chosen `r` is closer to `e` than I am, then `e` is reachable
*through* `r` — the edge would be redundant, pointing into a direction already
covered. Skip it.

The result is a **diverse** neighbourhood: edges spanning different directions
rather than clustering on one side. That diversity is exactly what greedy
search needs, because greedy search only works if there is an edge pointing
roughly toward wherever the query happens to be.

This is why HNSW beats a plain k-nearest-neighbour graph, and it is the part
people most often simplify away — with a measurable cost in recall.

## The three knobs

| | what it does | costs | when you change it |
|---|---|---|---|
| **M** | neighbours per node | memory, build time | rarely — it is baked in |
| **efConstruction** | candidates considered while inserting | build time only | build once, query often → raise it |
| **efSearch** | candidates considered while searching | query latency | **at runtime, per query** |

The asymmetry is the useful part. `efSearch` is a live dial: the same index
answers a fast approximate query and a slow accurate one. `M` and
`efConstruction` are properties of the graph, and changing them means rebuilding.

## Deleting

Awkward, and the honest answer is that HNSW does not really support it.

Removing a node means finding every node that points at it and repairing those
lists. Worse, the removed node may have been the only bridge between two
regions, and cutting it can disconnect the graph — silently, with recall
collapsing for whatever ended up on the wrong side.

So VectorDB **tombstones**: the node stays in the graph and keeps routing
traffic, but is filtered out of results. Cheap, safe, and it means deleted
vectors still occupy memory. Periodic rebuild reclaims them.

There is a test that deletes half the vectors and confirms recall stays above
0.85 — the evidence that a tombstone still earns its keep as a router.

## What HNSW costs you

- **Memory.** `N × M × 2 × 4` bytes of edges, on top of the vectors. At N = 1M
  and M = 16 that is 128 MB, next to 3 GB of vectors.
- **Build time.** Roughly one search per inserted vector.
- **Exactness.** It is approximate, and it fails silently. Measure recall.
- **Random access.** Traversal jumps around memory unpredictably — the opposite
  of brute force's perfectly sequential scan. It touches far less data, but
  every touch is a potential cache miss.

That last point is why HNSW's advantage is smaller than the "0.1% of vectors
visited" figure suggests, and why it is not automatically the right choice for
small collections.

## Experiments

1. Set `M = 2` and measure recall. Then `M = 64`. Plot recall and
   `index_bytes()` against M and find your own knee.
2. Set `efConstruction = M` (the minimum) and compare recall against
   `efConstruction = 500` at the *same* `efSearch`. You are measuring graph
   quality with search effort held constant.
3. Replace the heuristic in `select_neighbours` with "take the closest M" and
   re-run the recall suite. This is the most instructive single-line change in
   the codebase.
4. Log how many nodes each search visits. Compare with N. That ratio is what
   you bought.
5. Delete 90% of the vectors and re-measure recall. Find the point where
   tombstones stop being viable and a rebuild is required.

## Where this lives in the code

- `src/index/hnsw/hnsw_index.cpp`:
  - `random_level()` — the geometric layer assignment
  - `greedy_descend()` — phase 1, width 1
  - `search_layer()` — phase 2, width `ef`
  - `select_neighbours()` — the diversity heuristic
  - `link()` — the reverse edge, and re-pruning when a list is full
- `include/vectordb/index/hnsw_config.hpp` — the three knobs and their trades

---

Next: [04-hnsw-algorithm.md](04-hnsw-algorithm.md)
