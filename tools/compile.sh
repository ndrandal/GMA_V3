#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build-debug}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Use Clang if available for best static analysis; fallback to GCC.
if command -v clang++ >/dev/null 2>&1; then
  export CXX=clang++
else
  export CXX=g++
fi

cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DGMA_STRICT=ON ..  # if you have an option gate; otherwise ignore
cmake --build . -j"$(nproc || sysctl -n hw.ncpu || echo 4)"
