#pragma once
#include "balancing/IBackendSelector.hpp"
#include "transport/Connection.hpp"
#include "transport/EpollReactor.hpp"
#include "transport/Socket.hpp"
#include <functional>
#include <memory>

namespace proxy {

class ProxySession : public std::enable_shared_from_this<ProxySession> {
public:
    static void start(transport::EpollReactor& reactor,
                       transport::Socket client_socket,
                       balancing::IBackendSelector& selector,
                       std::function<void()> on_closed);

private:
    ProxySession(transport::EpollReactor& reactor,
                 transport::Socket client_socket,
                 transport::Socket backend_socket,
                 std::function<void()> on_closed);

    void await_backend_connect();
    void on_backend_connect_ready(int fd);
    void begin_proxying();
    void on_event(int fd, uint32_t events);
    void close_session();

    struct Endpoint {
        transport::Socket socket;
        bool read_finished = false;
        uint32_t registered_interest = 0; // 0 = not registered
    };

    Endpoint& endpoint_for(int fd);
    Endpoint& peer_of(const Endpoint& endpoint);
    bool pump_events(Endpoint& endpoint, uint32_t events);
    uint32_t wanted_interest(const Endpoint& endpoint);
    void sync_interest(Endpoint& endpoint);

    transport::EpollReactor& reactor_;
    Endpoint client_;
    Endpoint backend_;
    transport::Connection connection_;
    std::function<void()> on_closed_;
    bool closed_ = false;
};

} // namespace proxy
