#pragma once
#include <atomic>
#include <cstdint>
#include <thread>

namespace test_support {

// echo server, one connection at a time
class MockBackend {
public:
    explicit MockBackend(uint16_t port);
    ~MockBackend();

    uint16_t port() const { return port_; }
    void stop();

private:
    void serve_loop();

    uint16_t port_;
    int listen_fd_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

} // namespace test_support
