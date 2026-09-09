#include <CLI/CLI.hpp>
#include <meminfo/common/logging.h>
#include <meminfo/common/config.h>
#include <meminfo/memory/memory_daemon.h>
#include <iostream>

using namespace meminfo;
using namespace meminfo::memory;

int main(int argc, char** argv) {
    CLI::App app{"MemInfo Memory Daemon"};
    
    std::string config_path = "/etc/meminfo/memoryd.toml";
    app.add_option("-c,--config", config_path, "Path to configuration file");
    
    std::string log_level = "info";
    app.add_option("-l,--log-level", log_level, "Log level (trace, debug, info, warn, error)");
    
    CLI11_PARSE(app, argc, argv);
    
    try {
        init_logging("memoryd", "/var/log/meminfo", log_level);
        spdlog::info("memoryd starting up...");
        
        Config config;
        try {
            config = Config(config_path);
            spdlog::info("Loaded config from {}", config_path);
        } catch (const std::exception& e) {
            spdlog::warn("Failed to load config, using defaults. Error: {}", e.what());
        }
        
        MemoryDaemon daemon(config);
        daemon.run();
        
        spdlog::info("memoryd shutting down cleanly.");
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
