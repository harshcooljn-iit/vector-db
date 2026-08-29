#!/usr/bin/env bash
# Tombstones, and getting the space back.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

DB="$OUT/tombstones.vdb"

step "Create a database with 200 vectors"
run "$VECTORDB" create "$DB" --dimension 8 --metric l2 --quiet
"$VECTORDB" generate --output "$OUT/data.vecs" --dimension 8 --count 200 --seed 5 --quiet
run "$VECTORDB" import "$DB" --input "$OUT/data.vecs" --start-id 0

step "Delete 150 of them"
for ID in $(seq 0 149); do
    "$VECTORDB" delete "$DB" "$ID" --quiet
done
echo "deleted 150"

step "The slots are still there — deletion is a tombstone"
run "$VECTORDB" stats "$DB"
echo
echo "Removing a graph node properly would mean repairing every neighbour list"
echo "that points at it, and — worse — the node may be the only bridge between"
echo "two regions. Cutting it can disconnect the graph silently. So the node"
echo "stays, keeps routing traffic, and is filtered out of results."

step "check notices and says what to do"
run "$VECTORDB" check "$DB"

step "Search still works, and never returns a deleted vector"
run "$VECTORDB" search "$DB" --query-file "$OUT/data.vecs" --k 5

step "Compact to reclaim the space"
run "$VECTORDB" compact "$DB"

step "Slots now equal live vectors, and check is clean"
run "$VECTORDB" stats "$DB"
run "$VECTORDB" check "$DB"

step "VectorIds are unchanged by compaction"
echo "Compaction renumbers internal slots; the ids you inserted with are"
echo "untouched. That is the whole reason VectorId and LocalId are separate."
run "$VECTORDB" get "$DB" 199 --preview 4
