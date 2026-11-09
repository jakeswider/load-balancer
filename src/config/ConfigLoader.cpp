#include "config/ConfigLoader.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace config {

namespace {

void require_field(const nlohmann::json& j, const std::string& field) {
    if (!j.contains(field)) {
        throw ConfigError("missing required field: " + field);
    }
}

} // namespace

Config ConfigLoader::load_from_string(const std::string& json_text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::parse_error& e) {
        throw ConfigError(std::string("malformed JSON: ") + e.what());
    }

    require_field(j, "listen_address");
    require_field(j, "listen_port");
    require_field(j, "shard_count");
    require_field(j, "backends");

    Config cfg;
    cfg.listen_address = j.at("listen_address").get<std::string>();
    cfg.listen_port = j.at("listen_port").get<uint16_t>();
    cfg.shard_count = j.at("shard_count").get<int>();
    cfg.backends = j.at("backends").get<std::vector<std::string>>();

    if (cfg.shard_count <= 0) {
        throw ConfigError("shard_count must be positive, got " + std::to_string(cfg.shard_count));
    }
    if (cfg.backends.empty()) {
        throw ConfigError("backends list must not be empty");
    }

    return cfg;
}

Config ConfigLoader::load_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw ConfigError("could not open config file: " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return load_from_string(buffer.str());
}

} // namespace config
