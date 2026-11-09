#pragma once
#include "config/Config.hpp"
#include "transport/Shard.hpp"
#include "balancing/RoundRobinSelector.hpp"
#include <memory>
#include <thread>
#include <vector>

namespace bootstrap {

class Application {
public:
    explicit Application(config::Config config);

    // blocks until every shard drains after request_shutdown()
    void run();

    // async-signal-safe
    void request_shutdown();

private:
    config::Config config_;
    std::vector<std::unique_ptr<balancing::RoundRobinSelector>> selectors_;
    std::vector<std::unique_ptr<transport::Shard>> shards_;
    std::vector<std::thread> shard_threads_;
};

} // namespace bootstrap
