#include "support/MockBackend.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace test_support {

MockBackend::MockBackend(uint16_t port) : port_(port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listen_fd_, 16);

    if (port_ == 0) {
        socklen_t len = sizeof(addr);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
    }

    thread_ = std::thread([this]() { serve_loop(); });
}

MockBackend::~MockBackend() {
    stop();
}

void MockBackend::stop() {
    if (running_.exchange(false)) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

void MockBackend::serve_loop() {
    while (running_) {
        int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            break;
        }
        char buf[4096];
        ssize_t n;
        while ((n = ::read(client_fd, buf, sizeof(buf))) > 0) {
            ::send(client_fd, buf, n, MSG_NOSIGNAL);
        }
        ::close(client_fd);
    }
}

} // namespace test_support
