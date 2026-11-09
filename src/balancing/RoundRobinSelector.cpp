#include "balancing/RoundRobinSelector.hpp"

namespace balancing {

RoundRobinSelector::RoundRobinSelector(std::vector<std::string> backends)
    : backends_(std::move(backends)) {}

std::string RoundRobinSelector::next_backend() {
    const std::string& backend = backends_[next_index_];
    next_index_ = (next_index_ + 1) % backends_.size();
    return backend;
}

} // namespace balancing
