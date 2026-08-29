#!/usr/bin/env bash
# Generating, importing and exporting at a realistic size.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

DB="$OUT/bulk.vdb"
N=20000
D=128

step "Generate $N clustered vectors of dimension $D"
run "$VECTORDB" generate --output "$OUT/data.vecs" --dimension $D --count $N --seed 11
ls -lh "$OUT/data.vecs"

step "The same data as CSV, for comparison"
run "$VECTORDB" generate --output "$OUT/data.csv" --dimension $D --count 100 --seed 11 --csv
head -c 200 "$OUT/data.csv"; echo " ..."
echo
echo "CSV is roughly 12x larger and needs parsing. Use it for examples,"
echo "never for bulk data."

step "Import"
run "$VECTORDB" create "$DB" --dimension $D --metric l2 --quiet
run "$VECTORDB" import "$DB" --input "$OUT/data.vecs"

step "Stats"
run "$VECTORDB" stats "$DB"

step "Batch search"
run "$VECTORDB" generate --output "$OUT/queries.vecs" --dimension $D --count 20 --seed 11 --sample-seed 777 --quiet
run "$VECTORDB" batch-search "$DB" --queries "$OUT/queries.vecs" --k 3 --time

step "Export, then round-trip into a fresh database"
run "$VECTORDB" export "$DB" --output "$OUT/exported.vecs"
run "$VECTORDB" create "$OUT/roundtrip.vdb" --dimension $D --metric l2 --quiet
run "$VECTORDB" import "$OUT/roundtrip.vdb" --input "$OUT/exported.vecs"
echo
echo "The exported file carries ids, so the round-trip preserves them."
