#include "proxy/ProxySession.hpp"
#include <string>
#include <utility>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace proxy {

namespace {

std::pair<std::string, uint16_t> parse_address(const std::string& backend) {
    auto colon = backend.find(':');
    return {backend.substr(0, colon), static_cast<uint16_t>(std::stoi(backend.substr(colon + 1)))};
}

} // namespace

void ProxySession::start(transport::EpollReactor& reactor,
                          transport::Socket client_socket,
                          balancing::IBackendSelector& selector,
                          std::function<void()> on_closed) {
    auto [address, port] = parse_address(selector.next_backend());

    transport::Socket backend_socket;
    try {
        backend_socket = transport::Socket::connect_tcp(address, port);
    } catch (const std::exception&) {
        // still report the close so the shard's connection count stays balanced
        if (on_closed) {
            on_closed();
        }
        return;
    }

    // kept alive by the shared_ptr copies in its reactor handlers
    auto session = std::shared_ptr<ProxySession>(
        new ProxySession(reactor, std::move(client_socket), std::move(backend_socket), std::move(on_closed)));
    session->await_backend_connect();
}

ProxySession::ProxySession(transport::EpollReactor& reactor,
                            transport::Socket client_socket,
                            transport::Socket backend_socket,
                            std::function<void()> on_closed)
    : reactor_(reactor),
      client_{std::move(client_socket)},
      backend_{std::move(backend_socket)},
      connection_(client_.socket.fd(), backend_.socket.fd()),
      on_closed_(std::move(on_closed)) {}

void ProxySession::await_backend_connect() {
    auto self = shared_from_this();
    // writable once connect() finishes, SO_ERROR says if it worked
    reactor_.add_fd(backend_.socket.fd(), EPOLLOUT,
                     [self](int fd, uint32_t) { self->on_backend_connect_ready(fd); });
}

void ProxySession::on_backend_connect_ready(int fd) {
    // remove_fd() drops the handler holding the last ref to this session
    auto self = shared_from_this();

    if (closed_) {
        return;
    }
    reactor_.remove_fd(fd);

    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        close_session();
        return;
    }
    begin_proxying();
}

void ProxySession::begin_proxying() {
    sync_interest(client_);
    sync_interest(backend_);
}

ProxySession::Endpoint& ProxySession::endpoint_for(int fd) {
    return fd == client_.socket.fd() ? client_ : backend_;
}

ProxySession::Endpoint& ProxySession::peer_of(const Endpoint& endpoint) {
    return &endpoint == &client_ ? backend_ : client_;
}

void ProxySession::on_event(int fd, uint32_t events) {
    auto self = shared_from_this();

    if (closed_) {
        return;
    }
    if (!pump_events(endpoint_for(fd), events) || (client_.read_finished && backend_.read_finished)) {
        close_session();
        return;
    }
    sync_interest(client_);
    sync_interest(backend_);
}

bool ProxySession::pump_events(Endpoint& endpoint, uint32_t events) {
    if (events & EPOLLERR) {
        return false;
    }
    int fd = endpoint.socket.fd();
    if ((events & EPOLLOUT) && connection_.flush_pending(fd) != transport::PumpResult::Ok) {
        return false;
    }
    if (!(events & (EPOLLIN | EPOLLHUP)) || !(endpoint.registered_interest & EPOLLIN)) {
        return true;
    }

    switch (connection_.pump_from(fd)) {
        case transport::PumpResult::Ok:
            return true;
        case transport::PumpResult::PeerClosed:
            // half-close, the other direction might still be sending a response
            endpoint.read_finished = true;
            peer_of(endpoint).socket.shutdown_write();
            return true;
        case transport::PumpResult::Error:
            return false;
    }
    return false;
}

uint32_t ProxySession::wanted_interest(const Endpoint& endpoint) {
    int fd = endpoint.socket.fd();
    int peer_fd = peer_of(endpoint).socket.fd();

    uint32_t interest = 0;
    // stop reading while the peer is backed up so tcp flow control pushes back
    // on the sender instead of us buffering forever
    if (!endpoint.read_finished && !connection_.has_pending_write_to(peer_fd)) {
        interest |= EPOLLIN;
    }
    if (connection_.has_pending_write_to(fd)) {
        interest |= EPOLLOUT;
    }
    return interest;
}

void ProxySession::sync_interest(Endpoint& endpoint) {
    uint32_t wanted = wanted_interest(endpoint);
    if (wanted == endpoint.registered_interest) {
        return;
    }

    int fd = endpoint.socket.fd();
    if (endpoint.registered_interest == 0) {
        auto self = shared_from_this();
        reactor_.add_fd(fd, wanted, [self](int ready_fd, uint32_t events) { self->on_event(ready_fd, events); });
    } else if (wanted == 0) {
        // epoll still reports EPOLLHUP with an empty mask, which would busy-loop
        reactor_.remove_fd(fd);
    } else {
        reactor_.modify_fd(fd, wanted);
    }
    endpoint.registered_interest = wanted;
}

void ProxySession::close_session() {
    if (closed_) {
        return;
    }
    auto self = shared_from_this();

    closed_ = true;
    reactor_.remove_fd(client_.socket.fd());
    reactor_.remove_fd(backend_.socket.fd());
    if (on_closed_) {
        on_closed_();
    }
}

} // namespace proxy
