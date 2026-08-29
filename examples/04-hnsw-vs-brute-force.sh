#!/usr/bin/env bash
# The same data in both index types.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

N=20000
D=128

step "Generate $N vectors of dimension $D"
run "$VECTORDB" generate --output "$OUT/data.vecs" --dimension $D --count $N --seed 42
run "$VECTORDB" generate --output "$OUT/query.vecs" --dimension $D --count 1 --seed 42 --sample-seed 999

step "Build a brute-force database (exact, no index to construct)"
run "$VECTORDB" create "$OUT/exact.vdb" --dimension $D --metric l2 --index brute_force --quiet
time "$VECTORDB" import "$OUT/exact.vdb" --input "$OUT/data.vecs"

step "Build an HNSW database (approximate, pays a build cost)"
run "$VECTORDB" create "$OUT/ann.vdb" --dimension $D --metric l2 --index hnsw --quiet
time "$VECTORDB" import "$OUT/ann.vdb" --input "$OUT/data.vecs"

step "Exact search"
run "$VECTORDB" search "$OUT/exact.vdb" --query-file "$OUT/query.vecs" --k 10 --time

step "Approximate search over the same data"
run "$VECTORDB" search "$OUT/ann.vdb" --query-file "$OUT/query.vecs" --k 10 --time

step "What HNSW costs in memory"
"$VECTORDB" stats "$OUT/exact.vdb" | sed -n '/Memory/,/^$/p'
"$VECTORDB" stats "$OUT/ann.vdb" | sed -n '/Memory/,/^$/p'

echo
echo "Compare the two result lists above: they should be identical or nearly so."
echo "HNSW trades a build cost and ~145 bytes/vector of graph for a large drop"
echo "in query latency. At N=20,000 measured elsewhere: 24x faster at"
echo "recall@10 = 0.976. See docs/benchmark-results.md."
