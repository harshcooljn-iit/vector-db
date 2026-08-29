#!/usr/bin/env bash
# Typed metadata, filters, and the honest limit of post-filtering.
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

DB="$OUT/papers.vdb"

step "Create a database and insert papers with metadata"
run "$VECTORDB" create "$DB" --dimension 3 --metric cosine --quiet

# Metadata is typed by shape: 2026 becomes an integer, so 'year >= 2020' works.
# Quote it to force text.
"$VECTORDB" insert "$DB" --id 1 --vector "1,0,0"   --meta title=rates    --meta category=finance --meta year=2026 --meta score=0.91 --quiet
"$VECTORDB" insert "$DB" --id 2 --vector "0.9,0.1,0" --meta title=banks  --meta category=finance --meta year=2019 --meta score=0.72 --quiet
"$VECTORDB" insert "$DB" --id 3 --vector "0.8,0.2,0" --meta title=frogs  --meta category=biology --meta year=2026 --meta score=0.88 --quiet
"$VECTORDB" insert "$DB" --id 4 --vector "0,1,0"   --meta title=markets  --meta category=finance --meta year=2024 --meta score=0.65 --quiet
echo "inserted 4 papers"

step "No filter — pure nearest neighbours"
run "$VECTORDB" search "$DB" --query "1,0,0" --k 4

step "Filter by an exact string"
run "$VECTORDB" search "$DB" --query "1,0,0" --k 4 --filter 'category == "finance"'

step "Filter by a numeric range"
echo "year was stored as an integer, so >= compares numerically."
run "$VECTORDB" search "$DB" --query "1,0,0" --k 4 --filter 'year >= 2024'

step "Combine conditions with 'and'"
run "$VECTORDB" search "$DB" --query "1,0,0" --k 4 --filter 'category == "finance" and year >= 2024'

step "Floating-point comparison"
run "$VECTORDB" search "$DB" --query "1,0,0" --k 4 --filter 'score > 0.8'

step "A missing key never matches — including for !="
echo "None of these papers has a 'reviewed' key, so both filters return nothing."
run "$VECTORDB" search "$DB" --query "1,0,0" --k 4 --filter 'reviewed == 1'
run "$VECTORDB" search "$DB" --query "1,0,0" --k 4 --filter 'reviewed != 1'

step "There is no 'or' — and the error says so"
set +e
"$VECTORDB" search "$DB" --query "1,0,0" --filter 'a == 1 or b == 2'
echo "(exit code $?)"
set -e

step "Selective filters: why --exact exists"
echo "A post-filter over an approximate search can only keep what the index"
echo "returned. With a very selective filter the fetched window may contain"
echo "fewer than k matches even though more exist. --exact scans everything:"
echo "O(N), but it cannot miss. See docs/metadata.md."
run "$VECTORDB" search "$DB" --query "1,0,0" --k 2 --filter 'year == 2019' --exact
