# Exercise 5 — Break the neighbour heuristic

🔴 Advanced · [HNSW algorithm](../40-search-indexes/04-hnsw-algorithm.md)

**The single most instructive one-line change in this codebase.**

## Goal

Find out what HNSW's neighbour-selection heuristic is actually worth, by
deleting it.

## Background

When a new node has 200 candidates and may keep 16, the obvious choice is the 16
closest. HNSW does not do that. It accepts candidate `e` only if `e` is closer to
the new node than to anything already chosen.

The stated reason: a node at the edge of a dense cluster has all 16 of its
nearest neighbours *inside* that cluster, all in one direction. It becomes a
dead end — a search can enter but has no edge pointing anywhere useful.

That is the argument. Test it.

## Part A: delete it

In `src/index/hnsw/hnsw_index.cpp`, replace `select_neighbours` with a
take-the-closest version:

```cpp
void HnswIndex::select_neighbours(std::vector<Candidate>& candidates,
                                  std::size_t limit, std::vector<LocalId>& out) const {
    out.clear();
    std::sort(candidates.begin(), candidates.end(), kBetter);
    for (std::size_t i = 0; i < std::min(limit, candidates.size()); ++i) {
        out.push_back(candidates[i].local_id);
    }
}
```

Then:

```sh
cmake --build --preset release
./build/release/bin/vectordb_tests --gtest_filter='Hnsw*'
./build/release/bin/vectordb-bench hnsw
```

**Predict first.** Will the tests fail? Will recall drop? By how much? At which
`efSearch`?

## Part B: where does it hurt most?

Compare the two versions across `efSearch` from 5 to 400.

**Questions.** Is the gap constant, or does it close at high `efSearch`? What
does that tell you about *what* the heuristic fixes — is it making the graph
better, or making low-effort searches better?

## Part C: connectivity

Instrument both graphs. For each node, count how many other nodes link **to** it
(the reverse degree, which the graph does not store — you will have to compute
it).

**Questions.** How do the distributions differ? Are there nodes with zero
in-edges — unreachable except as an entry point? Which version has more?

## Part D: cluster structure

The effect should depend on how clustered the data is.

Compare `kClustered` (tight, `cluster_stddev = 0.15`), `kClustered` (loose,
1.0), and `kGaussian`.

**Questions.** Where does the heuristic matter most? Does that match the
argument in Part A's background — that it is protecting against dense clusters?

## Part E: half a heuristic

The real version has a second step: if the heuristic leaves the list short, fill
from the rejects in distance order.

Remove only that top-up, keeping the heuristic.

**Questions.** How full are the neighbour lists now? What happens to recall, and
to `index_bytes()`? Was the top-up doing anything?

## Then put it back

```sh
git checkout src/index/hnsw/hnsw_index.cpp
```

## Where to look

- `src/index/hnsw/hnsw_index.cpp` — `select_neighbours`, and `link` which reuses it
- `docs/hnsw.md` — the specification
- `tests/unit/index_hnsw_test.cpp` — the recall suite you are about to move
