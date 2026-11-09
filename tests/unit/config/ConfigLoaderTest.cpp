#include <gtest/gtest.h>
#include "config/ConfigLoader.hpp"

TEST(ConfigLoader, ParsesValidConfig) {
    std::string json = R"({
        "listen_address": "0.0.0.0",
        "listen_port": 8080,
        "shard_count": 4,
        "backends": ["127.0.0.1:9001", "127.0.0.1:9002"]
    })";

    config::Config cfg = config::ConfigLoader::load_from_string(json);

    EXPECT_EQ(cfg.listen_address, "0.0.0.0");
    EXPECT_EQ(cfg.listen_port, 8080);
    EXPECT_EQ(cfg.shard_count, 4);
    ASSERT_EQ(cfg.backends.size(), 2);
    EXPECT_EQ(cfg.backends[0], "127.0.0.1:9001");
    EXPECT_EQ(cfg.backends[1], "127.0.0.1:9002");
}

TEST(ConfigLoader, RejectsEmptyBackendList) {
    std::string json = R"({
        "listen_address": "0.0.0.0",
        "listen_port": 8080,
        "shard_count": 4,
        "backends": []
    })";

    EXPECT_THROW(config::ConfigLoader::load_from_string(json), config::ConfigError);
}

TEST(ConfigLoader, RejectsZeroShardCount) {
    std::string json = R"({
        "listen_address": "0.0.0.0",
        "listen_port": 8080,
        "shard_count": 0,
        "backends": ["127.0.0.1:9001"]
    })";

    EXPECT_THROW(config::ConfigLoader::load_from_string(json), config::ConfigError);
}

TEST(ConfigLoader, RejectsMissingField) {
    std::string json = R"({
        "listen_address": "0.0.0.0",
        "shard_count": 4,
        "backends": ["127.0.0.1:9001"]
    })";

    EXPECT_THROW(config::ConfigLoader::load_from_string(json), config::ConfigError);
}

TEST(ConfigLoader, RejectsMalformedJson) {
    std::string json = "{ not valid json";

    EXPECT_THROW(config::ConfigLoader::load_from_string(json), config::ConfigError);
}
