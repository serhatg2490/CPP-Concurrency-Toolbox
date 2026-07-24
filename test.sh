#!/usr/bin/env bash
# Run tests (and, by default, benchmarks) against an existing build.
# Run ./build.sh first if build/ doesn't exist yet.
#
# Usage:
#   ./test.sh              # unit tests + benchmarks (default, ~20s)
#   ./test.sh --unit-only  # unit tests only, skips the ~15s benchmark run

set -euo pipefail

if [[ ! -f build/CMakeCache.txt ]]; then
    echo "No build/ found -- run ./build.sh first." >&2
    exit 1
fi

if [[ "${1:-}" == "--unit-only" ]]; then
    ctest --test-dir build -C Release --output-on-failure -LE benchmark
else
    ctest --test-dir build -C Release --output-on-failure
fi
