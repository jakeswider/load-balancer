#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace config {

struct Config {
    std::string listen_address;
    uint16_t listen_port;
    int shard_count;
    std::vector<std::string> backends;
};

} // namespace config
