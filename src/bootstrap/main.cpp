#include "bootstrap/Application.hpp"
#include "config/ConfigLoader.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {
bootstrap::Application* g_app = nullptr;

void handle_sigterm(int) {
    if (g_app) {
        g_app->request_shutdown();
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <config.json>\n";
        return 1;
    }

    config::Config config;
    try {
        config = config::ConfigLoader::load_from_file(argv[1]);
    } catch (const config::ConfigError& e) {
        std::cerr << "invalid config: " << e.what() << "\n";
        return 1;
    }

    bootstrap::Application app(std::move(config));
    g_app = &app;

    std::signal(SIGTERM, handle_sigterm);
    std::signal(SIGINT, handle_sigterm);

    app.run();
    return 0;
}
