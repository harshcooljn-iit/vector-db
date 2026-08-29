#!/usr/bin/env bash
# Shared setup for the examples.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VECTORDB="$ROOT/build/release/bin/vectordb"
OUT="$ROOT/examples/out"

if [[ ! -x "$VECTORDB" ]]; then
    echo "vectordb not found at $VECTORDB" >&2
    echo "Build it first:  cmake --preset release && cmake --build --preset release" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

# Prints a section header, so the output reads as a narrative rather than a
# wall of commands.
step() {
    printf '\n\033[1m== %s ==\033[0m\n' "$1"
}

# Echoes a command before running it, so the reader can follow along.
run() {
    printf '\033[2m$ %s\033[0m\n' "$*"
    "$@"
}
