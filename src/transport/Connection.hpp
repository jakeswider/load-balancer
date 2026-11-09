#pragma once
#include <cstddef>
#include <vector>

namespace transport {

enum class PumpResult { Ok, PeerClosed, Error };

class Connection {
public:
    Connection(int side_a_fd, int side_b_fd);

    // forwards one read to the other side, buffers whatever can't be written yet
    PumpResult pump_from(int from_fd);
    PumpResult flush_pending(int to_fd);

    bool has_pending_write_to(int fd) const;

private:
    int other_side(int fd) const;
    std::vector<char>& buffer_for(int destination_fd);

    int side_a_fd_;
    int side_b_fd_;
    std::vector<char> pending_to_a_;
    std::vector<char> pending_to_b_;
};

} // namespace transport
