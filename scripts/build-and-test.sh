#!/usr/bin/env bash
# Configure, build and test a preset from scratch-ish. The everyday loop.
#   ./scripts/build-and-test.sh [preset]     default: debug
set -euo pipefail

cd "$(dirname "$0")/.."
preset="${1:-debug}"

: "${VCPKG_ROOT:?VCPKG_ROOT must point at your vcpkg checkout}"

cmake --preset "$preset"
cmake --build --preset "$preset"
ctest --preset "$preset"
