#!/usr/bin/env bash
# Build only (see test.sh to run unit tests / benchmarks afterward).
#
# Usage:
#   ./build.sh            # configure (if needed) + incremental build
#   ./build.sh --clean    # remove build/ first, then configure + build from scratch

set -euo pipefail

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf build
fi

if [[ ! -f build/CMakeCache.txt ]]; then
    cmake -B build -S .
fi

cmake --build build --config Release -j"$(nproc)"
