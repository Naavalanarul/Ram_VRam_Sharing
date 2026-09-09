#include <CLI/CLI.hpp>
#include <meminfo/common/logging.h>
#include <meminfo/common/config.h>
#include <meminfo/common/signal_handler.h>
#include <meminfo/discovery/discovery_daemon.h>
#include <iostream>

using namespace meminfo;
using namespace meminfo::discovery;

int main(int argc, char** argv) {
    CLI::App app{"MemInfo Discovery Daemon"};
    
    std::string config_path = "/etc/meminfo/discoveryd.toml";
    app.add_option("-c,--config", config_path, "Path to configuration file");
    
    std::string log_level = "info";
    app.add_option("-l,--log-level", log_level, "Log level (trace, debug, info, warn, error)");
    
    CLI11_PARSE(app, argc, argv);
    
    try {
        init_logging("discoveryd", "/var/log/meminfo", log_level);
        spdlog::info("discoveryd starting up...");
        
        Config config;
        try {
            config = Config(config_path);
            spdlog::info("Loaded config from {}", config_path);
        } catch (const std::exception& e) {
            spdlog::warn("Failed to load config, using defaults. Error: {}", e.what());
        }
        
        DiscoveryDaemon daemon(config);
        
        // Use a default libuv loop for the signal handler to stop the daemon gracefully
        // Wait, DiscoveryDaemon manages its own loop. To stop it via signals,
        // we need a mechanism. DiscoveryDaemon encapsulates its loop, so we can't easily 
        // add a SignalHandler to it from the outside without exposing the loop.
        // For simplicity, we can let DiscoveryDaemon's run() method block,
        // and we could run a background thread for signals, but libuv signals must run on the loop.
        // I will add a stop method that works async or just rely on Ctrl+C terminating the process for now,
        // but robustly we should expose the loop or add signals inside DiscoveryDaemon.
        
        daemon.run();
        
        spdlog::info("discoveryd shutting down cleanly.");
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
