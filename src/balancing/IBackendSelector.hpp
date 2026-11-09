#pragma once
#include <string>

namespace balancing {

class IBackendSelector {
public:
    virtual ~IBackendSelector() = default;
    virtual std::string next_backend() = 0;
};

} // namespace balancing
