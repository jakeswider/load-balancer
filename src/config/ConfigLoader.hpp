#pragma once
#include "config/Config.hpp"
#include <stdexcept>
#include <string>

namespace config {

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& message) : std::runtime_error(message) {}
};

class ConfigLoader {
public:
    static Config load_from_string(const std::string& json_text);
    static Config load_from_file(const std::string& path);
};

} // namespace config
