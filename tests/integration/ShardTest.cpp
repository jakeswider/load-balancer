#include <gtest/gtest.h>
#include "transport/Shard.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

// blocking on purpose, Socket::connect_tcp() is non-blocking and would hide ECONNREFUSED
bool connect_refused(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    bool refused = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 && errno == ECONNREFUSED;
    ::close(fd);
    return refused;
}

} // namespace

TEST(Shard, InvokesCallbackOnAcceptedConnection) {
    std::atomic<bool> accepted{false};

    transport::Shard shard("127.0.0.1", 0, [&](transport::EpollReactor&, transport::Socket client) {
        EXPECT_GE(client.fd(), 0);
        accepted = true;
        shard.notify_connection_closed();
    });

    uint16_t port = shard.listen_port();
    std::thread shard_thread([&shard]() { shard.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto client = transport::Socket::connect_tcp("127.0.0.1", port);

    for (int attempt = 0; attempt < 100 && !accepted; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(accepted.load());

    shard.request_shutdown();
    shard_thread.join();
}

TEST(Shard, RunReturnsAfterShutdownWithNoConnections) {
    transport::Shard shard("127.0.0.1", 0, [](transport::EpollReactor&, transport::Socket) {});

    std::thread shard_thread([&shard]() { shard.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    shard.request_shutdown();

    shard_thread.join();
}

TEST(Shard, ClosesListenSocketOnShutdown) {
    std::atomic<bool> accepted{false};

    // never closes, so the shard stays draining
    transport::Shard shard("127.0.0.1", 0, [&](transport::EpollReactor&, transport::Socket) {
        accepted = true;
    });

    uint16_t port = shard.listen_port();
    std::thread shard_thread([&shard]() { shard.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto held = transport::Socket::connect_tcp("127.0.0.1", port);

    for (int attempt = 0; attempt < 100 && !accepted; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(accepted.load());

    shard.request_shutdown();

    bool refused = false;
    for (int attempt = 0; attempt < 100 && !refused; ++attempt) {
        refused = connect_refused(port);
        if (!refused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(refused) << "expected the listening socket to be closed once shutdown was requested";

    shard.notify_connection_closed();
    shard_thread.join();
}
