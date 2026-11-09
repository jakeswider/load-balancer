#include "bootstrap/Application.hpp"
#include "proxy/ProxySession.hpp"

namespace bootstrap {

Application::Application(config::Config config) : config_(std::move(config)) {
    for (int i = 0; i < config_.shard_count; ++i) {
        selectors_.push_back(std::make_unique<balancing::RoundRobinSelector>(config_.backends));
        balancing::RoundRobinSelector* selector = selectors_.back().get();

        shards_.push_back(std::make_unique<transport::Shard>(
            config_.listen_address, config_.listen_port, transport::Shard::AcceptHandler{}));

        // handler needs the shard's address, so it has to be set after construction
        transport::Shard* shard = shards_.back().get();
        shard->set_accept_handler(
            [selector, shard](transport::EpollReactor& reactor, transport::Socket client) {
                proxy::ProxySession::start(reactor, std::move(client), *selector,
                                            [shard]() { shard->notify_connection_closed(); });
            });
    }
}

void Application::run() {
    for (auto& shard : shards_) {
        transport::Shard* shard_ptr = shard.get();
        shard_threads_.emplace_back([shard_ptr]() { shard_ptr->run(); });
    }
    for (auto& t : shard_threads_) {
        t.join();
    }
}

void Application::request_shutdown() {
    for (auto& shard : shards_) {
        shard->request_shutdown();
    }
}

} // namespace bootstrap
