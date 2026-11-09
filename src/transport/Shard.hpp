#pragma once
#include "transport/EpollReactor.hpp"
#include "transport/Socket.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace transport {

class Shard {
public:
    // every accepted connection needs a matching notify_connection_closed(),
    // otherwise run() never returns after shutdown
    using AcceptHandler = std::function<void(EpollReactor&, Socket client_socket)>;

    Shard(std::string listen_address, uint16_t listen_port, AcceptHandler on_accept);

    uint16_t listen_port() const { return listen_socket_.local_port(); }

    void set_accept_handler(AcceptHandler handler) { on_accept_ = std::move(handler); }

    // blocks until shutdown is requested and every connection has closed
    void run();

    // thread-safe and async-signal-safe
    void request_shutdown();

    void notify_connection_closed();

    bool is_running() const { return running_; }

private:
    void on_listen_readable(int fd, uint32_t events);
    void on_shutdown_readable(int fd, uint32_t events);

    std::string listen_address_;
    Socket listen_socket_;
    EpollReactor reactor_;
    AcceptHandler on_accept_;
    int shutdown_eventfd_;
    bool accepting_ = true;
    bool running_ = false;
    int open_connection_count_ = 0;
};

} // namespace transport
