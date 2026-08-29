#!/usr/bin/env bash
# The recall/latency dial, turned by hand.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

DB="$OUT/dial.vdb"
D=64
N=20000

step "Build an HNSW database with a deliberately small M"
echo "M=4 starves the graph, so efSearch has something visible to fix."
run "$VECTORDB" create "$DB" --dimension $D --metric l2 --index hnsw --m 4 --ef-construction 20 --quiet
"$VECTORDB" generate --output "$OUT/data.vecs" --dimension $D --count $N --seed 3 --quiet
run "$VECTORDB" import "$DB" --input "$OUT/data.vecs"
"$VECTORDB" generate --output "$OUT/query.vecs" --dimension $D --count 1 --seed 3 --sample-seed 4242 --quiet

step "Ground truth: an exact scan"
run "$VECTORDB" search "$DB" --query-file "$OUT/query.vecs" --k 5 --exact --time

step "Now the same query at increasing efSearch"
for EF in 1 2 5 10 50 200; do
    printf '\n--- efSearch=%s ---\n' "$EF"
    "$VECTORDB" search "$DB" --query-file "$OUT/query.vecs" --k 5 --ef-search "$EF" --time
done

echo
echo "Compare each result list against the exact one at the top."
echo "Low efSearch explores less of the graph: faster, and more likely to miss."
echo "Raising it converges on the exact answer and costs latency."
echo
echo "efSearch is a *runtime* dial — the same index serves a fast approximate"
echo "query and a slow accurate one. M and efConstruction are baked into the"
echo "graph and changing them means a rebuild."
