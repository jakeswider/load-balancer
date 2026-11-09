#!/usr/bin/env bash
set -euo pipefail

IMAGE=load-balancer-dev
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

docker build -t "$IMAGE" "$REPO_ROOT"

docker run --rm -v "$REPO_ROOT:/workspace" -w /workspace "$IMAGE" bash -c "
  set -euo pipefail
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
  cmake --build build
  ctest --test-dir build --output-on-failure
"
