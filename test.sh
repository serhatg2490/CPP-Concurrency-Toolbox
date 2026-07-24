#!/usr/bin/env bash
# Run tests (and, by default, benchmarks) against an existing build.
# Run ./build.sh first if build/ doesn't exist yet.
#
# Usage:
#   ./test.sh              # unit tests (quiet unless failing) + benchmarks (full output), ~20s
#   ./test.sh --unit-only  # unit tests only, skips the ~15s benchmark run

set -euo pipefail

if [[ ! -f build/CMakeCache.txt ]]; then
    echo "No build/ found -- run ./build.sh first." >&2
    exit 1
fi

# Unit tests: quiet unless something fails.
ctest --test-dir build -C Release --output-on-failure -LE benchmark

if [[ "${1:-}" != "--unit-only" ]]; then
    # Benchmarks: always show full output -- the whole point of running them
    # is to see the latency numbers, not just confirm they didn't crash.
    ctest --test-dir build -C Release --verbose -L benchmark
fi
