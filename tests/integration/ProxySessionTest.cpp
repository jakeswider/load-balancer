#include <gtest/gtest.h>
#include "proxy/ProxySession.hpp"
#include "balancing/RoundRobinSelector.hpp"
#include "transport/EpollReactor.hpp"
#include "transport/Socket.hpp"
#include "support/MockBackend.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// timeouts so a stalled proxy fails the test instead of hanging it
int connect_client(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    timeval timeout{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

// depends on position so lost or reordered chunks can't match by accident
char pattern_byte(size_t position) {
    return static_cast<char>((position / 251) ^ (position % 251));
}

std::vector<char> pattern(size_t size) {
    std::vector<char> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = pattern_byte(i);
    }
    return bytes;
}

bool write_all(int fd, const std::vector<char>& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::vector<char> read_until_end(int fd, size_t limit) {
    std::vector<char> received(limit);
    size_t got = 0;
    while (got < limit) {
        ssize_t n = ::read(fd, received.data() + got, limit - got);
        if (n <= 0) {
            break;
        }
        got += static_cast<size_t>(n);
    }
    received.resize(got);
    return received;
}

} // namespace

class ProxySessionFlowTest : public ::testing::Test {
protected:
    void SetUp() override {
        reactor_thread = std::thread([this]() {
            while (!stop) {
                int fd = listener.accept();
                if (fd >= 0) {
                    proxy::ProxySession::start(reactor, transport::Socket(fd), selector,
                                                [this]() { closed = true; });
                }
                reactor.poll_once(5);
            }
        });
    }

    void TearDown() override {
        stop = true;
        reactor_thread.join();
    }

    bool closed_within(std::chrono::milliseconds limit) {
        auto deadline = std::chrono::steady_clock::now() + limit;
        while (!closed && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return closed;
    }

    test_support::MockBackend backend{0};
    balancing::RoundRobinSelector selector{{"127.0.0.1:" + std::to_string(backend.port())}};
    transport::EpollReactor reactor;
    transport::Socket listener = transport::Socket::listen_tcp("127.0.0.1", 0);
    std::atomic<bool> stop{false};
    std::atomic<bool> closed{false};
    std::thread reactor_thread;
};

TEST_F(ProxySessionFlowTest, DeliversEveryByteToAClientThatReadsLate) {
    constexpr size_t kSize = 32 * 1024 * 1024; // larger than every socket buffer on the path
    const std::vector<char> sent = pattern(kSize);
    int client = connect_client(listener.local_port());
    ASSERT_GE(client, 0);

    std::thread writer([&]() { write_all(client, sent); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::vector<char> received = read_until_end(client, kSize);
    ::shutdown(client, SHUT_RDWR);
    writer.join();
    ::close(client);

    ASSERT_EQ(received.size(), kSize);
    EXPECT_TRUE(received == sent);
}

TEST_F(ProxySessionFlowTest, StopsAcceptingDataWhileTheReceiverIsNotReading) {
    constexpr size_t kOffered = 256 * 1024 * 1024;
    // room for kernel buffers on all four hops, anything past this is proxy memory
    constexpr size_t kMaxInFlight = 128 * 1024 * 1024;

    int client = connect_client(listener.local_port());
    ASSERT_GE(client, 0);
    ::fcntl(client, F_SETFL, ::fcntl(client, F_GETFL, 0) | O_NONBLOCK);

    std::vector<char> chunk(64 * 1024, 'x');
    size_t accepted = 0;
    auto last_progress = std::chrono::steady_clock::now();
    while (accepted < kOffered &&
           std::chrono::steady_clock::now() - last_progress < std::chrono::milliseconds(500)) {
        ssize_t n = ::send(client, chunk.data(), chunk.size(), MSG_NOSIGNAL);
        if (n > 0) {
            accepted += static_cast<size_t>(n);
            last_progress = std::chrono::steady_clock::now();
        } else {
            ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK) << "proxy dropped the connection";
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ::close(client);

    EXPECT_LT(accepted, kMaxInFlight);
}

TEST_F(ProxySessionFlowTest, DeliversResponseAfterClientHalfCloses) {
    constexpr size_t kSize = 64 * 1024;
    const std::vector<char> sent = pattern(kSize);
    int client = connect_client(listener.local_port());
    ASSERT_GE(client, 0);

    ASSERT_TRUE(write_all(client, sent));
    ::shutdown(client, SHUT_WR);

    // asking for one extra byte also checks EOF, MockBackend only closes after
    // the half-close gets forwarded
    std::vector<char> received = read_until_end(client, kSize + 1);
    ::close(client);

    ASSERT_EQ(received.size(), kSize);
    EXPECT_TRUE(received == sent);
    EXPECT_TRUE(closed_within(std::chrono::seconds(2)));
}

TEST(ProxySession, ProxiesDataToBackendAndBack) {
    test_support::MockBackend backend(0);
    balancing::RoundRobinSelector selector({"127.0.0.1:" + std::to_string(backend.port())});

    transport::EpollReactor reactor;
    auto listener = transport::Socket::listen_tcp("127.0.0.1", 0);
    uint16_t proxy_port = listener.local_port();

    std::atomic<bool> closed{false};
    std::thread reactor_thread([&]() {
        for (int i = 0; i < 200 && !closed; ++i) {
            int fd = listener.accept();
            if (fd >= 0) {
                proxy::ProxySession::start(reactor, transport::Socket(fd), selector,
                                            [&closed]() { closed = true; });
            }
            reactor.poll_once(10);
        }
    });

    auto client = transport::Socket::connect_tcp("127.0.0.1", proxy_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const char* message = "ping";
    ::write(client.fd(), message, std::strlen(message));

    char buf[64] = {};
    ssize_t n = -1;
    for (int attempt = 0; attempt < 100 && n <= 0; ++attempt) {
        n = ::read(client.fd(), buf, sizeof(buf));
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    EXPECT_GT(n, 0);
    if (n > 0) {
        EXPECT_EQ(std::string(buf, n), message);
    }

    client.close();
    reactor_thread.join();
}

TEST(ProxySession, ClosesGracefullyWhenBackendConnectionRefused) {
    // grab an ephemeral port and release it so the connect gets refused
    test_support::MockBackend backend(0);
    uint16_t dead_port = backend.port();
    backend.stop();

    balancing::RoundRobinSelector selector({"127.0.0.1:" + std::to_string(dead_port)});

    transport::EpollReactor reactor;
    auto listener = transport::Socket::listen_tcp("127.0.0.1", 0);
    uint16_t proxy_port = listener.local_port();

    auto client = transport::Socket::connect_tcp("127.0.0.1", proxy_port);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int accepted_fd = listener.accept();
    ASSERT_GE(accepted_fd, 0);

    std::atomic<bool> closed{false};
    EXPECT_NO_THROW(proxy::ProxySession::start(reactor, transport::Socket(accepted_fd), selector,
                                                [&closed]() { closed = true; }));

    for (int attempt = 0; attempt < 200 && !closed; ++attempt) {
        reactor.poll_once(10);
    }

    EXPECT_TRUE(closed.load());
}
