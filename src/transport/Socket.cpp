#include "transport/Socket.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace transport {

namespace {

void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// nagle + delayed acks can stall multi-write messages by ~40ms
void disable_nagle(int fd) {
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

sockaddr_in make_addr(const std::string& address, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid address: " + address);
    }
    return addr;
}

} // namespace

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

Socket::~Socket() {
    close();
}

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Socket Socket::listen_tcp(const std::string& address, uint16_t port, int backlog) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
    }

    int reuseport = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuseport, sizeof(reuseport));

    sockaddr_in addr = make_addr(address, port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("bind() failed: ") + std::strerror(errno));
    }
    if (::listen(fd, backlog) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("listen() failed: ") + std::strerror(errno));
    }

    set_nonblocking(fd);
    return Socket(fd);
}

Socket Socket::connect_tcp(const std::string& address, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
    }
    // non-blocking so a slow backend can't stall the shard,
    // caller waits for EPOLLOUT and checks SO_ERROR
    set_nonblocking(fd);
    disable_nagle(fd);

    sockaddr_in addr = make_addr(address, port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 && errno != EINPROGRESS) {
        ::close(fd);
        throw std::runtime_error(std::string("connect() failed: ") + std::strerror(errno));
    }

    return Socket(fd);
}

uint16_t Socket::local_port() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}

int Socket::accept() {
    int accepted = ::accept(fd_, nullptr, nullptr);
    if (accepted < 0) {
        return -1;
    }
    set_nonblocking(accepted);
    disable_nagle(accepted);
    return accepted;
}

void Socket::shutdown_write() {
    ::shutdown(fd_, SHUT_WR);
}

} // namespace transport
