#include <CLI/CLI.hpp>
#include <meminfo/gpu/gpu_daemon.h>
#include <meminfo/common/logging.h>
#include <meminfo/common/signal_handler.h>
#include <iostream>

using namespace meminfo;

int main(int argc, char** argv) {
    CLI::App app{"MemInfo GPU Daemon (gpud)"};
    
    std::string config_file = "/etc/meminfo/gpud.toml";
    app.add_option("-c,--config", config_file, "Path to config file");
    
    std::string log_level = "info";
    app.add_option("-l,--log-level", log_level, "Log level (trace, debug, info, warn, error)");
    
    CLI11_PARSE(app, argc, argv);
    
    try {
        init_logging("gpud", "logs", log_level);
        
        Config config;
        try {
            config = Config(config_file);
        } catch (const std::exception& e) {
            spdlog::warn("Could not load config file {}, using defaults. Error: {}", config_file, e.what());
        }
        
        gpu::GpuDaemon daemon(config);
        
        uv_loop_t sig_loop;
        uv_loop_init(&sig_loop);
        SignalHandler sig_handler(&sig_loop, [&]() {
            daemon.stop();
        });
        
        std::thread sig_thread([&]() {
            uv_run(&sig_loop, UV_RUN_DEFAULT);
            uv_loop_close(&sig_loop);
        });
        
        daemon.run();
        
        uv_stop(&sig_loop);
        if (sig_thread.joinable()) {
            sig_thread.join();
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
