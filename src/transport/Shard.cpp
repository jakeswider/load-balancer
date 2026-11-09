#include "transport/Shard.hpp"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace transport {

Shard::Shard(std::string listen_address, uint16_t listen_port, AcceptHandler on_accept)
    : listen_address_(std::move(listen_address)),
      listen_socket_(Socket::listen_tcp(listen_address_, listen_port)),
      on_accept_(std::move(on_accept)),
      shutdown_eventfd_(::eventfd(0, EFD_NONBLOCK)) {}

void Shard::on_listen_readable(int /*fd*/, uint32_t /*events*/) {
    while (accepting_) {
        int client_fd = listen_socket_.accept();
        if (client_fd < 0) {
            break;
        }
        ++open_connection_count_;
        on_accept_(reactor_, Socket(client_fd));
    }
}

void Shard::on_shutdown_readable(int fd, uint32_t /*events*/) {
    uint64_t value;
    ::read(fd, &value, sizeof(value));
    accepting_ = false;
    reactor_.remove_fd(listen_socket_.fd());
    listen_socket_.close();
}

void Shard::run() {
    running_ = true;
    reactor_.add_fd(listen_socket_.fd(), EPOLLIN,
                     [this](int fd, uint32_t events) { on_listen_readable(fd, events); });
    reactor_.add_fd(shutdown_eventfd_, EPOLLIN,
                     [this](int fd, uint32_t events) { on_shutdown_readable(fd, events); });

    while (accepting_ || open_connection_count_ > 0) {
        reactor_.poll_once(100); // bounded so a drained count is noticed
    }

    reactor_.remove_fd(shutdown_eventfd_);
    ::close(shutdown_eventfd_);
    running_ = false;
}

void Shard::request_shutdown() {
    uint64_t one = 1;
    ::write(shutdown_eventfd_, &one, sizeof(one));
}

void Shard::notify_connection_closed() {
    --open_connection_count_;
}

} // namespace transport
