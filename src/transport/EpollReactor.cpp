#include "transport/EpollReactor.hpp"
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace transport {

EpollReactor::EpollReactor() {
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        throw std::runtime_error(std::string("epoll_create1() failed: ") + std::strerror(errno));
    }
}

EpollReactor::~EpollReactor() {
    ::close(epoll_fd_);
}

void EpollReactor::add_fd(int fd, uint32_t events, Handler handler) {
    handlers_[fd] = std::move(handler);
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::runtime_error(std::string("epoll_ctl(ADD) failed: ") + std::strerror(errno));
    }
}

void EpollReactor::modify_fd(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        throw std::runtime_error(std::string("epoll_ctl(MOD) failed: ") + std::strerror(errno));
    }
}

void EpollReactor::remove_fd(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    handlers_.erase(fd);
}

bool EpollReactor::poll_once(int timeout_ms) {
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, timeout_ms);
    if (n <= 0) {
        return false; // timeout or EINTR
    }

    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        auto it = handlers_.find(fd);
        if (it != handlers_.end()) {
            it->second(fd, events[i].events);
        }
    }
    return true;
}

} // namespace transport
