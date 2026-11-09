#pragma once
#include <cstdint>
#include <string>

namespace transport {

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    ~Socket();

    static Socket listen_tcp(const std::string& address, uint16_t port, int backlog = 128);
    static Socket connect_tcp(const std::string& address, uint16_t port);

    int fd() const { return fd_; }
    uint16_t local_port() const;
    int accept(); // -1 if nothing is pending
    void shutdown_write();
    void close();

private:
    int fd_ = -1;
};

} // namespace transport
