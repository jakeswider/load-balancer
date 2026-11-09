#include "transport/Connection.hpp"
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

namespace transport {

namespace {

// without this a SIGPIPE from one dead peer kills the whole process
ssize_t send_without_sigpipe(int fd, const char* data, size_t size) {
    return ::send(fd, data, size, MSG_NOSIGNAL);
}

} // namespace

Connection::Connection(int side_a_fd, int side_b_fd)
    : side_a_fd_(side_a_fd), side_b_fd_(side_b_fd) {}

int Connection::other_side(int fd) const {
    return fd == side_a_fd_ ? side_b_fd_ : side_a_fd_;
}

std::vector<char>& Connection::buffer_for(int destination_fd) {
    return destination_fd == side_a_fd_ ? pending_to_a_ : pending_to_b_;
}

bool Connection::has_pending_write_to(int fd) const {
    return fd == side_a_fd_ ? !pending_to_a_.empty() : !pending_to_b_.empty();
}

PumpResult Connection::pump_from(int from_fd) {
    int to_fd = other_side(from_fd);

    char buf[4096];
    ssize_t n = ::read(from_fd, buf, sizeof(buf));
    if (n == 0) {
        return PumpResult::PeerClosed;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return PumpResult::Ok;
        }
        return PumpResult::Error;
    }

    ssize_t written = send_without_sigpipe(to_fd, buf, static_cast<size_t>(n));
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            written = 0;
        } else {
            return PumpResult::Error;
        }
    }

    if (written < n) {
        auto& pending = buffer_for(to_fd);
        pending.insert(pending.end(), buf + written, buf + n);
    }

    return PumpResult::Ok;
}

PumpResult Connection::flush_pending(int to_fd) {
    auto& pending = buffer_for(to_fd);
    if (pending.empty()) {
        return PumpResult::Ok;
    }

    ssize_t written = send_without_sigpipe(to_fd, pending.data(), pending.size());
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return PumpResult::Ok;
        }
        return PumpResult::Error;
    }

    pending.erase(pending.begin(), pending.begin() + written);
    return PumpResult::Ok;
}

} // namespace transport
