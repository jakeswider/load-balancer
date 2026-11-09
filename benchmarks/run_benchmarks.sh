#!/usr/bin/env bash
# runs inside the container, each process is pinned to its own cpus
set -uo pipefail

BACKEND_CPUS=4,5
CLIENT_CPUS=6,7
LB_PORT=8080
BACKEND_PORTS=(9001 9002 9003)
RAW=/results/raw.txt
TOOL=/build/lb_bench
LB=/build/release/src/bootstrap/load_balancer

build() {
  cmake -S /workspace -B /build/release -G Ninja -DCMAKE_BUILD_TYPE=Release >/build/cmake.log 2>&1 \
    && cmake --build /build/release >>/build/cmake.log 2>&1 \
    || { tail -30 /build/cmake.log; exit 1; }
  g++-13 -std=c++20 -O2 -pthread -Wall -Wextra /bench/lb_bench.cpp -o "$TOOL" || exit 1
}

cpu_seconds() {
  awk '{printf "%.2f", ($14+$15)/100}' "/proc/$1/stat" 2>/dev/null || echo 0
}

start_backend() {
  taskset -c "$BACKEND_CPUS" "$TOOL" backend --ports "$(IFS=,; echo "${BACKEND_PORTS[*]}")" --threads 2 \
    >/tmp/backend.out 2>&1 &
  BACKEND_PID=$!
  sleep 0.3
}

stop_backend() {
  kill -TERM "$BACKEND_PID" 2>/dev/null
  wait "$BACKEND_PID" 2>/dev/null
  BACKEND_ACCEPTS=$(cat /tmp/backend.out)
}

start_lb() {
  local shards=$1
  cat >/tmp/lb.json <<EOF
{"listen_address":"127.0.0.1","listen_port":$LB_PORT,"shard_count":$shards,
 "backends":["127.0.0.1:${BACKEND_PORTS[0]}","127.0.0.1:${BACKEND_PORTS[1]}","127.0.0.1:${BACKEND_PORTS[2]}"]}
EOF
  taskset -c "0-$((shards - 1))" "$LB" /tmp/lb.json >/tmp/lb.out 2>&1 &
  LB_PID=$!
  sleep 0.3
  if ! kill -0 "$LB_PID" 2>/dev/null; then
    echo "load balancer failed to start: $(cat /tmp/lb.out)" >&2
  fi
}

stop_lb() {
  if kill -0 "$LB_PID" 2>/dev/null; then
    kill -TERM "$LB_PID"
    wait "$LB_PID"
    local code=$?
    LB_STATUS=$([ "$code" = 0 ] && echo ok || echo "exit_$code")
  else
    wait "$LB_PID"
    LB_STATUS="died_before_stop_exit_$?"
  fi
}

emit() {
  echo "phase=$PHASE $*" | tee -a "$RAW"
}

# run_case <direct|shardsN> <trial> <client mode> [args...]
run_case() {
  local label=$1 trial=$2
  shift 2
  local target="127.0.0.1:$LB_PORT" shards=0 extra=""
  start_backend
  if [ "$label" = direct ]; then
    target="127.0.0.1:${BACKEND_PORTS[0]}"
  else
    shards=${label#shards}
    start_lb "$shards"
  fi
  local cpu_backend_0 cpu_lb_0=0 wall_0
  cpu_backend_0=$(cpu_seconds "$BACKEND_PID")
  [ "$shards" -gt 0 ] && cpu_lb_0=$(cpu_seconds "$LB_PID")
  wall_0=$(date +%s.%N)

  local output
  output=$(taskset -c "$CLIENT_CPUS" "$TOOL" "$1" --target "$target" "${@:2}")

  local cpu_lb_1=$cpu_lb_0
  [ "$shards" -gt 0 ] && cpu_lb_1=$(cpu_seconds "$LB_PID")
  local cpu_backend_1 wall_1
  cpu_backend_1=$(cpu_seconds "$BACKEND_PID")
  wall_1=$(date +%s.%N)
  local cpu_report
  cpu_report=$(awk -v a="$cpu_lb_0" -v b="$cpu_lb_1" -v c="$cpu_backend_0" -v d="$cpu_backend_1" \
    -v s="$wall_0" -v e="$wall_1" 'BEGIN{printf "lb_cpu_cores=%.2f backend_cpu_cores=%.2f",(b-a)/(e-s),(d-c)/(e-s)}')

  LB_STATUS=none
  [ "$shards" -gt 0 ] && stop_lb
  stop_backend
  emit "mode=$1 config=$label trial=$trial $output $cpu_report lb_status=$LB_STATUS $BACKEND_ACCEPTS"
}

phase_saturation() {
  for trial in 1 2 3 4 5; do
    for label in direct shards1 shards2 shards4; do
      run_case "$label" "$trial" rr --connections 128 --threads 2 --size 1024 --warmup 3 --duration 15
    done
  done
}

phase_latency() {
  local rate=$1
  for trial in 1 2 3 4 5; do
    for label in direct shards4; do
      run_case "$label" "$trial" rr --connections 128 --threads 2 --size 1024 --warmup 3 --duration 15 --rate "$rate"
    done
  done
}

phase_unloaded() {
  for trial in 1 2 3 4 5; do
    for label in direct shards1; do
      run_case "$label" "$trial" rr --connections 1 --threads 1 --size 1024 --warmup 2 --duration 10
    done
  done
}

phase_connrate() {
  for trial in 1 2 3; do
    for label in direct shards4; do
      run_case "$label" "$trial" connrate --threads 16 --warmup 2 --duration 10
      sleep 2
    done
  done
}

phase_payload() {
  for size in 4096 16384 65536; do
    for trial in 1 2 3; do
      for label in direct shards4; do
        run_case "$label" "$trial" rr --connections 16 --threads 2 --size "$size" --warmup 2 --duration 8
      done
    done
  done
}

phase_integrity() {
  for trial in 1 2 3; do
    for label in direct shards1 shards4; do
      run_case "$label" "$trial" bulk --bytes 268435456
    done
  done
  for trial in 1 2 3; do
    for label in direct shards1 shards4; do
      run_case "$label" "$trial" bulk --bytes 268435456 --reader_delay_ms 1000
    done
  done
  for label in direct shards1 shards4; do
    run_case "$label" 1 halfclose --iterations 1000 --size 1024
  done
  for trial in 1 2 3; do
    run_case shards4 "$trial" abort --threads 8 --duration 10
  done
}

phase_shutdown() {
  for trial in 1 2 3 4 5; do
    start_backend
    start_lb 4
    taskset -c "$CLIENT_CPUS" "$TOOL" rr --target "127.0.0.1:$LB_PORT" --connections 128 --threads 2 \
      --size 1024 --warmup 0 --duration 8 >/tmp/client.out &
    local client_pid=$!
    sleep 3
    kill -TERM "$LB_PID"
    local refused=0
    sleep 0.3
    (exec 3<>"/dev/tcp/127.0.0.1/$LB_PORT") 2>/dev/null || refused=1
    wait "$client_pid"
    local client_done
    client_done=$(date +%s%N)
    wait "$LB_PID"
    local code=$?
    local exit_ms=$(( ($(date +%s%N) - client_done) / 1000000 ))
    stop_backend
    emit "mode=shutdown config=shards4 trial=$trial $(cat /tmp/client.out) new_conn_refused=$refused lb_exit_code=$code lb_exit_ms_after_clients_done=$exit_ms"
  done
}

phase_disconnect() {
  for size in 1024 16384 65536; do
    for trial in $(seq 1 20); do
      run_case shards4 "$trial" rr --connections 16 --threads 2 --size "$size" --warmup 0 --duration 2
    done
  done
}

PHASE=${1:?usage: run_benchmarks.sh <phase> [args]}
shift
build
"phase_$PHASE" "$@"
