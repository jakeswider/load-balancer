# load-balancer

A layer 4 TCP load balancer in C++20. Each shard runs its own epoll loop and SO_REUSEPORT listener,
spreads connections across backends with round-robin, and drains open connections on SIGTERM.

## Benchmarks

Apple M1, Docker, loopback, 1 KiB echo.

| Shards | Requests/s |
|---|---|
| 1 | 217k |
| 2 | 346k |
| 4 | 362k |

53k new connections/s on 4 shards, and no failed requests during graceful shutdown under load.

## Build

Linux only. On macOS, `./docker/build.sh` builds it and runs the tests in a container.
