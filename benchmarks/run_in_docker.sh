#!/usr/bin/env bash
# usage: benchmarks/run_in_docker.sh <phase> [args...]
# ephemeral ports go above 8080/9001-9003 so a client can't grab 8080 first
set -euo pipefail

IMAGE=load-balancer-dev
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_ROOT="${SOURCE_ROOT:-$REPO_ROOT}"
RESULTS_DIR="${RESULTS_DIR:-$REPO_ROOT/benchmarks/results}"
BUILD_VOLUME="${BUILD_VOLUME:-lb-bench-build}"
mkdir -p "$RESULTS_DIR"

docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" "$REPO_ROOT"

docker run --rm \
  -v "$SOURCE_ROOT:/workspace:ro" \
  -v "$REPO_ROOT/benchmarks:/bench:ro" \
  -v "$RESULTS_DIR:/results" \
  -v "$BUILD_VOLUME:/build" \
  --ulimit nofile=200000:200000 \
  --sysctl net.ipv4.ip_local_port_range="10000 65000" \
  --sysctl net.ipv4.tcp_tw_reuse=1 \
  --sysctl net.core.somaxconn=4096 \
  "$IMAGE" bash /bench/run_benchmarks.sh "$@"
