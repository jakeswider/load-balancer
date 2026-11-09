// load generator and echo backend. shares no code with src/ and checks
// every payload byte
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <numeric>
#include <queue>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Args = std::map<std::string, std::string>;

uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string arg_str(const Args& args, const std::string& key, const std::string& fallback) {
    auto it = args.find(key);
    return it == args.end() ? fallback : it->second;
}

uint64_t arg_u64(const Args& args, const std::string& key, uint64_t fallback) {
    auto it = args.find(key);
    return it == args.end() ? fallback : std::stoull(it->second);
}

sockaddr_in parse_target(const std::string& target) {
    auto colon = target.find(':');
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(std::stoi(target.substr(colon + 1))));
    inet_pton(AF_INET, target.substr(0, colon).c_str(), &addr.sin_addr);
    return addr;
}

void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

void set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void set_timeouts(int fd, int seconds) {
    timeval tv{seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

int connect_blocking(const sockaddr_in& addr) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    set_nodelay(fd);
    return fd;
}

// depends on (seed, offset) so lost, duplicated or reordered chunks fail verification
void fill_stream(char* out, size_t size, uint64_t seed, uint64_t offset) {
    for (size_t i = 0; i < size; ++i) {
        uint64_t position = offset + i;
        uint64_t word = (position / 8 + seed + 1) * 0x9E3779B97F4A7C15ULL;
        word ^= word >> 29;
        out[i] = static_cast<char>(word >> ((position % 8) * 8));
    }
}

double percentile(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    size_t index = std::min(sorted.size() - 1, static_cast<size_t>(p * static_cast<double>(sorted.size())));
    return static_cast<double>(sorted[index]) / 1000.0;
}

void print_latency(const std::vector<uint64_t>& sorted) {
    double mean = sorted.empty()
                      ? 0
                      : std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size()) / 1000.0;
    std::printf(" mean_us=%.1f p50_us=%.1f p90_us=%.1f p99_us=%.1f p999_us=%.1f max_us=%.1f", mean,
                percentile(sorted, 0.50), percentile(sorted, 0.90), percentile(sorted, 0.99),
                percentile(sorted, 0.999), sorted.empty() ? 0 : static_cast<double>(sorted.back()) / 1000.0);
}

// backend

std::atomic<bool> g_stop{false};

struct EchoConn {
    int fd;
    std::vector<char> pending;
    size_t pending_offset = 0;
    bool peer_closed = false;
};

void backend_thread(const std::vector<uint16_t>& ports, std::vector<std::atomic<uint64_t>>& accepted) {
    int ep = epoll_create1(0);
    std::map<int, size_t> listeners;
    for (size_t i = 0; i < ports.size(); ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
        sockaddr_in addr = parse_target("127.0.0.1:" + std::to_string(ports[i]));
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(fd, 4096) < 0) {
            std::perror("backend bind/listen");
            std::exit(1);
        }
        set_nonblocking(fd);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
        listeners[fd] = i;
    }

    std::map<int, std::unique_ptr<EchoConn>> conns;
    std::vector<char> buf(64 * 1024);
    epoll_event events[256];

    auto close_conn = [&](int fd) {
        epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        conns.erase(fd);
    };
    auto set_interest = [&](int fd, uint32_t interest) {
        epoll_event ev{};
        ev.events = interest;
        ev.data.fd = fd;
        epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
    };

    while (!g_stop) {
        int n = epoll_wait(ep, events, 256, 100);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            auto listener = listeners.find(fd);
            if (listener != listeners.end()) {
                int client;
                while ((client = accept(fd, nullptr, nullptr)) >= 0) {
                    set_nonblocking(client);
                    set_nodelay(client);
                    accepted[listener->second]++;
                    conns[client] = std::make_unique<EchoConn>(EchoConn{client, {}, 0, false});
                    epoll_event ev{};
                    ev.events = EPOLLIN;
                    ev.data.fd = client;
                    epoll_ctl(ep, EPOLL_CTL_ADD, client, &ev);
                }
                continue;
            }
            auto found = conns.find(fd);
            if (found == conns.end()) {
                continue;
            }
            EchoConn& conn = *found->second;

            if (events[i].events & EPOLLOUT) {
                ssize_t w = write(fd, conn.pending.data() + conn.pending_offset,
                                  conn.pending.size() - conn.pending_offset);
                if (w < 0 && errno != EAGAIN) {
                    close_conn(fd);
                    continue;
                }
                conn.pending_offset += w > 0 ? static_cast<size_t>(w) : 0;
                if (conn.pending_offset == conn.pending.size()) {
                    conn.pending.clear();
                    conn.pending_offset = 0;
                    if (conn.peer_closed) {
                        close_conn(fd);
                    } else {
                        set_interest(fd, EPOLLIN);
                    }
                }
                continue;
            }

            ssize_t r = read(fd, buf.data(), buf.size());
            if (r == 0) {
                close_conn(fd);
            } else if (r < 0) {
                if (errno != EAGAIN) {
                    close_conn(fd);
                }
            } else {
                ssize_t w = write(fd, buf.data(), static_cast<size_t>(r));
                if (w < 0 && errno != EAGAIN) {
                    close_conn(fd);
                    continue;
                }
                size_t written = w > 0 ? static_cast<size_t>(w) : 0;
                if (written < static_cast<size_t>(r)) {
                    conn.pending.assign(buf.data() + written, buf.data() + r);
                    set_interest(fd, EPOLLOUT);
                }
            }
        }
    }
    close(ep);
}

int run_backend(const Args& args) {
    std::vector<uint16_t> ports;
    std::string list = arg_str(args, "ports", "9001");
    size_t start = 0;
    while (start < list.size()) {
        size_t comma = list.find(',', start);
        ports.push_back(static_cast<uint16_t>(std::stoi(list.substr(start, comma - start))));
        start = comma == std::string::npos ? list.size() : comma + 1;
    }
    std::signal(SIGTERM, [](int) { g_stop = true; });

    std::vector<std::atomic<uint64_t>> accepted(ports.size());
    std::vector<std::thread> threads;
    for (uint64_t i = 0; i < arg_u64(args, "threads", 2); ++i) {
        threads.emplace_back([&] { backend_thread(ports, accepted); });
    }
    for (auto& t : threads) {
        t.join();
    }
    std::printf("backend_accepts");
    for (size_t i = 0; i < ports.size(); ++i) {
        std::printf(" port_%u=%llu", ports[i], static_cast<unsigned long long>(accepted[i].load()));
    }
    std::printf("\n");
    return 0;
}

// request/response load

struct RrConn {
    int fd = -1;
    uint64_t id = 0;
    uint64_t stream_offset = 0;
    uint64_t scheduled_ns = 0;
    size_t sent = 0;
    size_t received = 0;
    bool in_flight = false;
    bool dead = false;
    std::vector<char> expected;
};

struct RrResult {
    std::vector<uint64_t> latencies;
    uint64_t corrupt = 0;
    uint64_t peer_closed = 0;
    uint64_t stalled = 0;
};

using Due = std::pair<uint64_t, RrConn*>;

void rr_worker(std::vector<RrConn>& conns, size_t size, uint64_t interval_ns, uint64_t measure_from,
               uint64_t measure_until, RrResult& result) {
    int ep = epoll_create1(0);
    for (auto& c : conns) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = &c;
        epoll_ctl(ep, EPOLL_CTL_ADD, c.fd, &ev);
    }

    auto set_interest = [&](RrConn& c, uint32_t interest) {
        epoll_event ev{};
        ev.events = interest;
        ev.data.ptr = &c;
        epoll_ctl(ep, EPOLL_CTL_MOD, c.fd, &ev);
    };
    auto continue_send = [&](RrConn& c) {
        ssize_t w = write(c.fd, c.expected.data() + c.sent, size - c.sent);
        if (w > 0) {
            c.sent += static_cast<size_t>(w);
        }
        if (w < 0 && errno != EAGAIN) {
            c.dead = true;
            result.peer_closed++;
            epoll_ctl(ep, EPOLL_CTL_DEL, c.fd, nullptr);
            return;
        }
        set_interest(c, c.sent < size ? EPOLLIN | EPOLLOUT : EPOLLIN);
    };
    auto begin_request = [&](RrConn& c, uint64_t scheduled) {
        c.scheduled_ns = scheduled;
        c.sent = 0;
        c.received = 0;
        c.in_flight = true;
        fill_stream(c.expected.data(), size, c.id, c.stream_offset);
        continue_send(c);
    };

    std::priority_queue<Due, std::vector<Due>, std::greater<>> due;
    uint64_t t0 = now_ns();
    for (size_t i = 0; i < conns.size(); ++i) {
        if (interval_ns == 0) {
            begin_request(conns[i], now_ns());
        } else {
            due.emplace(t0 + i * interval_ns / conns.size(), &conns[i]);
        }
    }

    std::vector<char> buf(size);
    epoll_event events[256];
    while (true) {
        uint64_t now = now_ns();
        if (now >= measure_until) {
            break;
        }
        while (!due.empty() && due.top().first <= now) {
            auto [when, conn] = due.top();
            due.pop();
            // measured from the intended send time to avoid coordinated omission
            begin_request(*conn, when);
        }
        int n = epoll_wait(ep, events, 256, interval_ns == 0 ? 50 : 0);
        for (int i = 0; i < n; ++i) {
            RrConn& c = *static_cast<RrConn*>(events[i].data.ptr);
            if (c.dead) {
                continue;
            }
            if ((events[i].events & EPOLLOUT) && c.in_flight && c.sent < size) {
                continue_send(c);
                if (c.dead) {
                    continue;
                }
            }
            if (!(events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))) {
                continue;
            }
            ssize_t r = read(c.fd, buf.data(), size - c.received);
            if (r < 0 && errno == EAGAIN) {
                continue;
            }
            if (r <= 0) {
                c.dead = true;
                result.peer_closed++;
                epoll_ctl(ep, EPOLL_CTL_DEL, c.fd, nullptr);
                continue;
            }
            if (std::memcmp(buf.data(), c.expected.data() + c.received, static_cast<size_t>(r)) != 0) {
                result.corrupt++;
                c.dead = true;
                epoll_ctl(ep, EPOLL_CTL_DEL, c.fd, nullptr);
                continue;
            }
            c.received += static_cast<size_t>(r);
            if (c.received == size) {
                uint64_t done = now_ns();
                if (c.scheduled_ns >= measure_from && done < measure_until) {
                    result.latencies.push_back(done - c.scheduled_ns);
                }
                c.in_flight = false;
                c.stream_offset += size;
                if (interval_ns == 0) {
                    begin_request(c, done);
                } else {
                    due.emplace(c.scheduled_ns + interval_ns, &c);
                }
            }
        }
    }

    uint64_t end = now_ns();
    for (auto& c : conns) {
        if (!c.dead && c.in_flight && end - c.scheduled_ns > 1'000'000'000ULL) {
            result.stalled++;
        }
    }
    close(ep);
}

int run_rr(const Args& args) {
    sockaddr_in addr = parse_target(arg_str(args, "target", "127.0.0.1:8080"));
    uint64_t connections = arg_u64(args, "connections", 128);
    uint64_t thread_count = arg_u64(args, "threads", 2);
    uint64_t size = arg_u64(args, "size", 1024);
    uint64_t warmup_s = arg_u64(args, "warmup", 3);
    uint64_t duration_s = arg_u64(args, "duration", 15);
    uint64_t rate = arg_u64(args, "rate", 0);

    std::vector<std::vector<RrConn>> per_thread(thread_count);
    for (uint64_t i = 0; i < connections; ++i) {
        RrConn c;
        c.fd = connect_blocking(addr);
        if (c.fd < 0) {
            std::perror("rr connect");
            return 1;
        }
        set_nonblocking(c.fd);
        c.id = i;
        c.expected.resize(size);
        per_thread[i % thread_count].push_back(std::move(c));
    }

    uint64_t interval_ns = rate == 0 ? 0 : connections * 1'000'000'000ULL / rate;
    uint64_t start = now_ns();
    uint64_t measure_from = start + warmup_s * 1'000'000'000ULL;
    uint64_t measure_until = measure_from + duration_s * 1'000'000'000ULL;

    std::vector<RrResult> results(thread_count);
    std::vector<std::thread> threads;
    for (uint64_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            rr_worker(per_thread[t], size, interval_ns, measure_from, measure_until, results[t]);
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    std::vector<uint64_t> latencies;
    uint64_t corrupt = 0;
    uint64_t peer_closed = 0;
    uint64_t stalled = 0;
    for (auto& r : results) {
        latencies.insert(latencies.end(), r.latencies.begin(), r.latencies.end());
        corrupt += r.corrupt;
        peer_closed += r.peer_closed;
        stalled += r.stalled;
    }
    std::sort(latencies.begin(), latencies.end());
    double rps = static_cast<double>(latencies.size()) / static_cast<double>(duration_s);
    std::printf("size=%llu connections=%llu requests=%zu rps=%.0f mbytes_per_s=%.1f corrupt=%llu peer_closed=%llu stalled=%llu",
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(connections), latencies.size(),
                rps, rps * static_cast<double>(size) / 1e6, static_cast<unsigned long long>(corrupt),
                static_cast<unsigned long long>(peer_closed), static_cast<unsigned long long>(stalled));
    print_latency(latencies);
    std::printf("\n");
    for (auto& v : per_thread) {
        for (auto& c : v) {
            close(c.fd);
        }
    }
    return 0;
}

// new-connection load

int run_connrate(const Args& args) {
    sockaddr_in addr = parse_target(arg_str(args, "target", "127.0.0.1:8080"));
    uint64_t thread_count = arg_u64(args, "threads", 16);
    uint64_t duration_s = arg_u64(args, "duration", 10);
    uint64_t warmup_s = arg_u64(args, "warmup", 2);
    constexpr size_t kSize = 64;

    uint64_t start = now_ns();
    uint64_t measure_from = start + warmup_s * 1'000'000'000ULL;
    uint64_t measure_until = measure_from + duration_s * 1'000'000'000ULL;

    std::vector<std::vector<uint64_t>> samples(thread_count);
    std::atomic<uint64_t> connect_failed{0};
    std::atomic<uint64_t> exchange_failed{0};
    std::atomic<uint64_t> corrupt{0};
    std::vector<std::thread> threads;
    for (uint64_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            char sent[kSize];
            char received[kSize];
            uint64_t seed = t << 32;
            while (true) {
                uint64_t begin = now_ns();
                if (begin >= measure_until) {
                    break;
                }
                int fd = connect_blocking(addr);
                if (fd < 0) {
                    connect_failed++;
                    continue;
                }
                set_timeouts(fd, 5);
                fill_stream(sent, kSize, seed++, 0);
                size_t got = 0;
                bool ok = write(fd, sent, kSize) == static_cast<ssize_t>(kSize);
                while (ok && got < kSize) {
                    ssize_t r = read(fd, received + got, kSize - got);
                    ok = r > 0;
                    got += r > 0 ? static_cast<size_t>(r) : 0;
                }
                close(fd);
                uint64_t done = now_ns();
                if (!ok) {
                    exchange_failed++;
                } else if (std::memcmp(sent, received, kSize) != 0) {
                    corrupt++;
                } else if (begin >= measure_from && done < measure_until) {
                    samples[t].push_back(done - begin);
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    std::vector<uint64_t> latencies;
    for (auto& s : samples) {
        latencies.insert(latencies.end(), s.begin(), s.end());
    }
    std::sort(latencies.begin(), latencies.end());
    std::printf("connections=%zu conns_per_s=%.0f connect_failed=%llu exchange_failed=%llu corrupt=%llu",
                latencies.size(), static_cast<double>(latencies.size()) / static_cast<double>(duration_s),
                static_cast<unsigned long long>(connect_failed.load()),
                static_cast<unsigned long long>(exchange_failed.load()),
                static_cast<unsigned long long>(corrupt.load()));
    print_latency(latencies);
    std::printf("\n");
    return 0;
}

// integrity probes

int run_bulk(const Args& args) {
    sockaddr_in addr = parse_target(arg_str(args, "target", "127.0.0.1:8080"));
    uint64_t bytes = arg_u64(args, "bytes", 256 * 1024 * 1024);
    uint64_t chunk = arg_u64(args, "chunk", 64 * 1024);
    uint64_t rate_mbps = arg_u64(args, "pace_mbytes_per_s", 0);

    int fd = connect_blocking(addr);
    if (fd < 0) {
        std::perror("bulk connect");
        return 1;
    }
    set_timeouts(fd, 5);

    std::atomic<uint64_t> sent{0};
    uint64_t begin = now_ns();
    std::thread writer([&] {
        std::vector<char> buf(chunk);
        while (sent < bytes) {
            size_t n = std::min<uint64_t>(chunk, bytes - sent);
            fill_stream(buf.data(), n, 7, sent);
            size_t off = 0;
            while (off < n) {
                ssize_t w = write(fd, buf.data() + off, n - off);
                if (w <= 0) {
                    return;
                }
                off += static_cast<size_t>(w);
            }
            sent += n;
            if (rate_mbps > 0) {
                uint64_t target = begin + sent * 1000 / rate_mbps;
                while (now_ns() < target) {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
        }
    });

    // delaying the reader fills every socket buffer on the path
    std::this_thread::sleep_for(std::chrono::milliseconds(arg_u64(args, "reader_delay_ms", 0)));

    uint64_t received = 0;
    uint64_t first_mismatch = UINT64_MAX;
    std::string outcome = "complete";
    std::vector<char> buf(chunk);
    std::vector<char> expected(chunk);
    uint64_t last_progress = now_ns();
    while (received < bytes) {
        ssize_t r = read(fd, buf.data(), chunk);
        if (r == 0) {
            outcome = "peer_closed";
            break;
        }
        if (r < 0) {
            outcome = errno == EAGAIN ? "stalled_5s_no_data" : "read_error";
            break;
        }
        fill_stream(expected.data(), static_cast<size_t>(r), 7, received);
        if (first_mismatch == UINT64_MAX && std::memcmp(buf.data(), expected.data(), static_cast<size_t>(r)) != 0) {
            for (ssize_t i = 0; i < r; ++i) {
                if (buf[i] != expected[i]) {
                    first_mismatch = received + static_cast<uint64_t>(i);
                    break;
                }
            }
        }
        received += static_cast<uint64_t>(r);
        last_progress = now_ns();
    }
    double elapsed_s = static_cast<double>(last_progress - begin) / 1e9;
    shutdown(fd, SHUT_RDWR);
    writer.join();
    close(fd);

    bool intact = received == bytes && first_mismatch == UINT64_MAX;
    std::printf("bytes_requested=%llu bytes_sent=%llu bytes_received=%llu intact=%d outcome=%s first_mismatch=%lld "
                "mbytes_per_s=%.1f\n",
                static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(sent.load()),
                static_cast<unsigned long long>(received), intact ? 1 : 0, outcome.c_str(),
                first_mismatch == UINT64_MAX ? -1LL : static_cast<long long>(first_mismatch),
                static_cast<double>(received) / elapsed_s / 1e6);
    return 0;
}

// send, shutdown(SHUT_WR), then expect the full echo before EOF
int run_halfclose(const Args& args) {
    sockaddr_in addr = parse_target(arg_str(args, "target", "127.0.0.1:8080"));
    uint64_t iterations = arg_u64(args, "iterations", 1000);
    uint64_t size = arg_u64(args, "size", 1024);

    uint64_t complete = 0;
    uint64_t truncated = 0;
    std::vector<char> sent(size);
    std::vector<char> received(size);
    for (uint64_t i = 0; i < iterations; ++i) {
        int fd = connect_blocking(addr);
        if (fd < 0) {
            truncated++;
            continue;
        }
        set_timeouts(fd, 2);
        fill_stream(sent.data(), size, i, 0);
        if (write(fd, sent.data(), size) != static_cast<ssize_t>(size)) {
            truncated++;
            close(fd);
            continue;
        }
        shutdown(fd, SHUT_WR);
        size_t got = 0;
        while (got < size) {
            ssize_t r = read(fd, received.data() + got, size - got);
            if (r <= 0) {
                break;
            }
            got += static_cast<size_t>(r);
        }
        if (got == size && std::memcmp(sent.data(), received.data(), size) == 0) {
            complete++;
        } else {
            truncated++;
        }
        close(fd);
    }
    std::printf("iterations=%llu complete=%llu truncated=%llu\n", static_cast<unsigned long long>(iterations),
                static_cast<unsigned long long>(complete), static_cast<unsigned long long>(truncated));
    return 0;
}

// clients RST while their echo is in flight
int run_abort(const Args& args) {
    sockaddr_in addr = parse_target(arg_str(args, "target", "127.0.0.1:8080"));
    uint64_t thread_count = arg_u64(args, "threads", 8);
    uint64_t duration_s = arg_u64(args, "duration", 10);
    uint64_t until = now_ns() + duration_s * 1'000'000'000ULL;

    std::atomic<uint64_t> cycles{0};
    std::atomic<uint64_t> connect_failed{0};
    std::vector<std::thread> threads;
    for (uint64_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&] {
            std::vector<char> buf(128 * 1024, 'x');
            while (now_ns() < until) {
                int fd = connect_blocking(addr);
                if (fd < 0) {
                    connect_failed++;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                set_nonblocking(fd);
                (void)!write(fd, buf.data(), buf.size());
                linger reset_on_close{1, 0};
                setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset_on_close, sizeof(reset_on_close));
                close(fd);
                cycles++;
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    std::printf("abort_cycles=%llu connect_failed=%llu\n", static_cast<unsigned long long>(cycles.load()),
                static_cast<unsigned long long>(connect_failed.load()));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s backend|rr|connrate|bulk|halfclose|abort [--key value ...]\n", argv[0]);
        return 2;
    }
    Args args;
    for (int i = 2; i + 1 < argc; i += 2) {
        args[std::string(argv[i]).substr(2)] = argv[i + 1];
    }
    std::string mode = argv[1];
    if (mode == "backend") return run_backend(args);
    if (mode == "rr") return run_rr(args);
    if (mode == "connrate") return run_connrate(args);
    if (mode == "bulk") return run_bulk(args);
    if (mode == "halfclose") return run_halfclose(args);
    if (mode == "abort") return run_abort(args);
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
}
