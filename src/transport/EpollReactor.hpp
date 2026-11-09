#pragma once
#include <sys/epoll.h>
#include <functional>
#include <unordered_map>

namespace transport {

class EpollReactor {
public:
    using Handler = std::function<void(int fd, uint32_t events)>;

    EpollReactor();
    ~EpollReactor();

    EpollReactor(const EpollReactor&) = delete;
    EpollReactor& operator=(const EpollReactor&) = delete;

    void add_fd(int fd, uint32_t events, Handler handler);
    void modify_fd(int fd, uint32_t events);
    void remove_fd(int fd);

    // false if nothing fired before the timeout
    bool poll_once(int timeout_ms);

private:
    int epoll_fd_;
    std::unordered_map<int, Handler> handlers_;
};

} // namespace transport
