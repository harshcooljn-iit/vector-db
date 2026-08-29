# Approximate nearest neighbour — trading correctness for speed

🟡 Intermediate

## The wall

Brute force is `O(N·D)` and memory-bandwidth-bound. At N = 1,000,000 and
D = 768 every query reads 3 GB. At ~50 GB/s that is 60 ms of pure memory
traffic, before any arithmetic, per query.

You want under a millisecond. No amount of SIMD closes a 60× gap on a loop that
is waiting for DRAM — you have to **touch less data**.

## Why the obvious structures fail

You know how to make search sub-linear in low dimensions: a k-d tree, a
quadtree, a grid. They all die in high dimensions, and it is worth knowing
precisely why.

### The curse of dimensionality

Take points spread uniformly in a `D`-dimensional unit cube. To capture a
fraction `f` of them, a hypercube must have side length `f^(1/D)`.

| D | side needed for 1% of points |
|---|---|
| 2 | 0.10 |
| 10 | 0.63 |
| 100 | **0.955** |
| 768 | **0.994** |

At D = 768, a region containing 1% of your data spans 99.4% of the range in
*every* dimension. There is no such thing as a small neighbourhood. A k-d tree
splits on one dimension at a time, so it needs to prune on ~768 dimensions to
isolate anything — and it ends up visiting nearly every leaf.

### Distances concentrate

Worse, the very notion of "nearest" weakens. For independent uniform points, as
`D` grows:

```
(distance to furthest point − distance to nearest point) / distance to nearest → 0
```

Everything is roughly equidistant. There is barely a nearest neighbour to find.

**This is not a hypothetical, and it bit this project.** The benchmark
generator originally drew query vectors from a different cluster layout than the
data, so every query landed in the empty space *between* clusters — exactly the
concentrated regime. A query's 10th nearest neighbour was 3% further than its
1st, versus 44% for a real in-distribution point. HNSW measured recall@10 =
0.752 on parameters that should be near-exact, and the graph was fine: the
benchmark was measuring tie-breaking. The fix is in
`generate_queries`; the commit message records the numbers.

**The lesson generalises**: if your ANN benchmark shows terrible recall, suspect
your data before your index.

### Why real embeddings are searchable anyway

Real embeddings are not uniform. They live on a low-dimensional manifold inside
the high-dimensional space — locally dense, globally sparse, genuinely
clustered. The *ambient* dimension is 768; the *intrinsic* dimension is far
lower. That is why ANN works in practice and why `DatasetKind::kClustered` is
the default generator.

## The bargain

Give up the guarantee.

An approximate index returns *probably* the nearest neighbours. Miss a few and
you get orders of magnitude in return.

For semantic search that is usually a free lunch. If you ask for papers about
interest rates, the 10th-best and 12th-best matches are both papers about
interest rates. The ranking was never exact to begin with — the embedding model
is itself an approximation of meaning.

For "find this exact fingerprint", it is not acceptable at all. Which is why
`vectordb` lets you choose the index.

## Measuring the loss: recall@k

```
recall@k = |returned top-k ∩ true top-k| / k
```

Set intersection, deliberately **not** position-by-position agreement.
Demanding the same *order* from an approximate algorithm gives you a test that
is either trivially weak or permanently flaky, and it measures the wrong thing:
two results at nearly equal distance are equally good answers.

Computing it requires ground truth, which means running brute force. That is
[the whole reason brute force stays](../../docs/decisions/ADR-0006-brute-force-first.md).

Typical operating points:

| recall@10 | means |
|---|---|
| 1.00 | exact; you are visiting everything |
| 0.99 | one result in ten queries differs, at the bottom of the list |
| 0.95 | usually indistinguishable for semantic search |
| 0.80 | noticeable; probably under-tuned |
| < 0.5 | something is wrong — check your *data* first |

## The families

### Trees (k-d, ball, VP)
Recursive space partitioning. Excellent below ~20 dimensions, useless above.
Annoy makes a forest of random-projection trees to compensate — many weak
partitions instead of one good one.

### Hashing (LSH)
Hash functions where nearby points collide. Strong theory, tunable failure
probability. In practice needs many hash tables and lots of memory to reach the
recall a graph gets cheaply.

### Quantization (PQ, IVF)
Do not touch less data — make the data smaller. Compress each vector to ~8-16
bytes, scan the compressed forms fast, rescore the top candidates exactly.
This is how billion-scale indexes fit in RAM, and it composes with everything
else. FAISS's speciality. Listed in [next-steps](../next-steps.md).

### Graphs (HNSW, NSG, DiskANN)
Connect near neighbours, then walk the edges greedily toward the query.
Best recall-per-microsecond at moderate scale, at the cost of memory for the
edges. **This is what we implement.**

The two axes are orthogonal, which is worth internalising: graphs reduce *how
many* vectors you touch; quantization reduces *how big* each one is. Production
systems do both.

## Why a graph works when trees do not

A tree asks "which region is the query in?" — and in high dimensions there are
no small regions.

A graph asks a different question: "given where I am, which of my neighbours is
closer to the query?" That is a purely *local* question. It never needs a
notion of region, so the curse of dimensionality has much less to bite on.

The catch is getting stuck: greedy descent in a single near-neighbour graph
reaches a local minimum, where no neighbour improves but the true answer is
elsewhere. That is precisely the problem HNSW's layers solve, and it is the
subject of the next note.

## Experiments

1. Generate 10,000 uniform points at D = 2, 10, 50, 200. For 100 random
   queries, compute `(furthest − nearest) / nearest`. Watch it collapse.
2. Repeat with `DatasetKind::kClustered`. The ratio stays large. You have just
   measured why real embeddings are searchable and uniform noise is not.
3. Run VectorDB's recall tests with `kUniform` instead of `kClustered`. Watch
   recall fall, then convince yourself the index did not change.
4. Estimate what recall@10 = 0.9 would cost you on a search task you care
   about. If you cannot tell, that is the answer.

## Where this lives in the code

- `include/vectordb/util/dataset.hpp` — `DatasetKind`, and the comment on why
  uniform data is a misleading default
- `src/util/dataset.cpp` — `generate_queries`, and the note recording the
  0.752-versus-0.999 bug
- `tests/support/recall.hpp` — `recall_at_k` as set intersection
- `tests/unit/index_hnsw_test.cpp` — the recall suite

---

Next: [03-hnsw-intuition.md](03-hnsw-intuition.md)
