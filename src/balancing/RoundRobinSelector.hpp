#pragma once
#include "balancing/IBackendSelector.hpp"
#include <vector>
#include <cstddef>

namespace balancing {

class RoundRobinSelector : public IBackendSelector {
public:
    explicit RoundRobinSelector(std::vector<std::string> backends);
    std::string next_backend() override;

private:
    std::vector<std::string> backends_;
    std::size_t next_index_ = 0;
};

} // namespace balancing
