#!/usr/bin/env bash
# The whole loop, small enough to follow by hand.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

DB="$OUT/quickstart.vdb"

step "Create a 4-dimensional database using cosine similarity"
run "$VECTORDB" create "$DB" --dimension 4 --metric cosine

step "Insert four vectors along the axes"
# Cosine ignores magnitude, so [2,0,0,0] and [1,0,0,0] point the same way.
run "$VECTORDB" insert "$DB" --id 1 --vector "1,0,0,0" --meta name=north
run "$VECTORDB" insert "$DB" --id 2 --vector "0,1,0,0" --meta name=east
run "$VECTORDB" insert "$DB" --id 3 --vector "2,0,0,0" --meta name=north-far
run "$VECTORDB" insert "$DB" --id 4 --vector "-1,0,0,0" --meta name=south

step "What is in there?"
run "$VECTORDB" info "$DB"

step "Search for the direction of 'north'"
echo "Expect: id 1 and id 3 tie at similarity 1.0 — cosine ignores magnitude,"
echo "so [1,0,0,0] and [2,0,0,0] are the same direction. id 4 is opposite (-1)."
run "$VECTORDB" search "$DB" --query "1,0,0,0" --k 4 --time

step "Fetch one vector by id"
run "$VECTORDB" get "$DB" 3

step "Delete it, and search again"
run "$VECTORDB" delete "$DB" 3
run "$VECTORDB" search "$DB" --query "1,0,0,0" --k 4

step "Machine-readable output"
run "$VECTORDB" search "$DB" --query "1,0,0,0" --k 2 --json

echo
echo "Done. The database is a directory:"
ls -la "$DB"
