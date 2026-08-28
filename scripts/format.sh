#!/usr/bin/env bash
# Format every source file we own. Run before committing.
#   ./scripts/format.sh          format in place
#   ./scripts/format.sh --check  fail if anything is unformatted (used by CI)
set -euo pipefail

cd "$(dirname "$0")/.."

# clang-format is not always on PATH on macOS even when installed, so look in
# the usual places before giving up.
CLANG_FORMAT="${CLANG_FORMAT:-}"
if [[ -z "$CLANG_FORMAT" ]]; then
    for candidate in clang-format \
                     /opt/homebrew/opt/llvm/bin/clang-format \
                     /usr/local/opt/llvm/bin/clang-format; do
        if command -v "$candidate" >/dev/null 2>&1; then
            CLANG_FORMAT="$candidate"
            break
        fi
    done
fi
if [[ -z "$CLANG_FORMAT" ]] && command -v xcrun >/dev/null 2>&1; then
    CLANG_FORMAT="$(xcrun -f clang-format 2>/dev/null || true)"
fi
if [[ -z "$CLANG_FORMAT" ]]; then
    echo "clang-format not found (brew install llvm / apt install clang-format)" >&2
    exit 1
fi

# Portable to bash 3.2, which is what macOS still ships as /bin/bash.
files=$(find include src tests benchmarks examples \
    -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) 2>/dev/null | sort)

if [[ -z "$files" ]]; then
    echo "no sources found"
    exit 0
fi

count=$(printf '%s\n' "$files" | wc -l | tr -d ' ')

if [[ "${1:-}" == "--check" ]]; then
    printf '%s\n' "$files" | xargs "$CLANG_FORMAT" --dry-run --Werror
    echo "format check passed ($count files)"
else
    printf '%s\n' "$files" | xargs "$CLANG_FORMAT" -i
    echo "formatted $count files"
fi
