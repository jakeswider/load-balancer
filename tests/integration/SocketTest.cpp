#include <gtest/gtest.h>
#include "transport/Socket.hpp"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

bool nagle_disabled(int fd) {
    int nodelay = 0;
    socklen_t len = sizeof(nodelay);
    ::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, &len);
    return nodelay != 0;
}

} // namespace

TEST(Socket, DisablesNagleOnConnectedAndAcceptedSockets) {
    auto listener = transport::Socket::listen_tcp("127.0.0.1", 0);
    auto client = transport::Socket::connect_tcp("127.0.0.1", listener.local_port());

    int accepted_fd = -1;
    for (int attempt = 0; attempt < 100 && accepted_fd < 0; ++attempt) {
        accepted_fd = listener.accept();
        if (accepted_fd < 0) {
            usleep(10 * 1000);
        }
    }
    ASSERT_GE(accepted_fd, 0);
    transport::Socket accepted(accepted_fd);

    EXPECT_TRUE(nagle_disabled(client.fd()));
    EXPECT_TRUE(nagle_disabled(accepted.fd()));
}

TEST(Socket, ListenConnectAcceptRoundTrip) {
    auto listener = transport::Socket::listen_tcp("127.0.0.1", 0);
    uint16_t port = listener.local_port();

    std::thread client_thread([port]() {
        auto client = transport::Socket::connect_tcp("127.0.0.1", port);
        ASSERT_TRUE(client.fd() >= 0);
    });

    int accepted_fd = -1;
    for (int attempt = 0; attempt < 100 && accepted_fd < 0; ++attempt) {
        accepted_fd = listener.accept();
        if (accepted_fd < 0) {
            usleep(10 * 1000);
        }
    }
    client_thread.join();

    EXPECT_GE(accepted_fd, 0);
    if (accepted_fd >= 0) {
        ::close(accepted_fd);
    }
}

TEST(Socket, MoveTransfersOwnership) {
    auto listener = transport::Socket::listen_tcp("127.0.0.1", 0);
    int original_fd = listener.fd();

    transport::Socket moved(std::move(listener));

    EXPECT_EQ(moved.fd(), original_fd);
    EXPECT_EQ(listener.fd(), -1); // NOLINT(bugprone-use-after-move)
}
