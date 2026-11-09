#include <gtest/gtest.h>
#include "transport/Connection.hpp"
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

// each side is its own socketpair, Connection holds the near end and the
// test plays the remote peer on the far end
class ConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        int client_fds[2];
        int backend_fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, client_fds), 0);
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, backend_fds), 0);
        client_near = client_fds[0];
        client_far = client_fds[1];
        backend_near = backend_fds[0];
        backend_far = backend_fds[1];
    }

    void TearDown() override {
        ::close(client_near);
        ::close(client_far);
        ::close(backend_near);
        ::close(backend_far);
    }

    int client_near;
    int client_far;
    int backend_near;
    int backend_far;
};

TEST_F(ConnectionTest, PumpsDataFromSideAToSideB) {
    transport::Connection conn(client_near, backend_near);

    const char* message = "hello backend";
    ::write(client_far, message, std::strlen(message));

    auto result = conn.pump_from(client_near);
    EXPECT_EQ(result, transport::PumpResult::Ok);

    char buf[64] = {};
    ssize_t n = ::read(backend_far, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf, n), message);
}

TEST_F(ConnectionTest, PumpsDataFromSideBToSideA) {
    transport::Connection conn(client_near, backend_near);

    const char* message = "hello client";
    ::write(backend_far, message, std::strlen(message));

    auto result = conn.pump_from(backend_near);
    EXPECT_EQ(result, transport::PumpResult::Ok);

    char buf[64] = {};
    ssize_t n = ::read(client_far, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string(buf, n), message);
}

TEST_F(ConnectionTest, ReportsPeerClosedOnEof) {
    transport::Connection conn(client_near, backend_near);
    ::close(client_far);

    auto result = conn.pump_from(client_near);

    EXPECT_EQ(result, transport::PumpResult::PeerClosed);
}

TEST_F(ConnectionTest, ReportsErrorWithoutRaisingSigpipeWhenDestinationIsGone) {
    transport::Connection conn(client_near, backend_near);
    ::close(backend_far);

    const char* message = "hello backend";
    ::write(client_far, message, std::strlen(message));

    auto result = conn.pump_from(client_near);

    EXPECT_EQ(result, transport::PumpResult::Error);
}
