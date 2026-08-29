#!/usr/bin/env bash
# Data outliving the process, and recovering from a destroyed index.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

DB="$OUT/persist.vdb"

step "Process 1: create and insert"
run "$VECTORDB" create "$DB" --dimension 8 --metric l2 --quiet
"$VECTORDB" generate --output "$OUT/data.vecs" --dimension 8 --count 500 --seed 7 --quiet
run "$VECTORDB" import "$DB" --input "$OUT/data.vecs"

step "The database is three files"
ls -la "$DB"
echo
echo "vectors.bin      authoritative float payload"
echo "index.hnsw       derived from it, and disposable"
echo "metadata.sqlite  metadata rows plus the database configuration"

step "Process 2 (a completely separate invocation): search"
run "$VECTORDB" search "$DB" --query-file "$OUT/data.vecs" --k 3 --time

step "Now destroy the index and reopen"
echo "The vector store is authoritative, so a corrupt index is not a reason to"
echo "fail — it is a reason to spend some CPU rebuilding."
echo "not an index any more" > "$DB/index.hnsw"
run "$VECTORDB" search "$DB" --query-file "$OUT/data.vecs" --k 3 --verbose

step "check confirms the database is healthy again"
run "$VECTORDB" check "$DB" --deep

step "Deleting the index entirely also recovers"
rm "$DB/index.hnsw"
run "$VECTORDB" search "$DB" --query-file "$OUT/data.vecs" --k 3
echo
echo "Loading a valid index takes ~9 ms at N=50,000; rebuilding takes ~7.8 s."
echo "That 860x gap is why the index is persisted at all."
