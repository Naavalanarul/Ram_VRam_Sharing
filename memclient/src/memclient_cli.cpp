#include <CLI/CLI.hpp>
#include <meminfo/client/client_api.h>
#include <meminfo/common/logging.h>
#include <iostream>

using namespace meminfo;
using namespace meminfo::client;

int main(int argc, char** argv) {
    CLI::App app{"MemInfo Memory Client CLI"};
    
    std::string discovery_sock = "/var/run/meminfo_discovery.sock";
    app.add_option("-s,--socket", discovery_sock, "Path to discoveryd control socket");
    
    size_t local_cache_size = 1024 * 1024; // 1MB default
    app.add_option("-m,--max-local", local_cache_size, "Max local cache size in bytes before evicting");
    
    CLI11_PARSE(app, argc, argv);
    
    try {
        init_logging("memclient", "logs", "info");
        
        spdlog::info("Initializing MemoryClient (local limit: {} bytes)...", local_cache_size);
        MemoryClient client(local_cache_size, discovery_sock);
        
        spdlog::info("Allocating 1024 bytes...");
        handle_t h = client.allocate(1024);
        spdlog::info("Got handle: {}", h);
        
        std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
        spdlog::info("Writing data to handle...");
        client.write(h, 0, data);
        
        spdlog::info("Reading data from handle...");
        auto read_data = client.read(h, 0, 4);
        
        spdlog::info("Read {} bytes. First byte: {:#x}", read_data.size(), read_data[0]);
        
        spdlog::info("Freeing handle...");
        client.free(h);
        
        spdlog::info("Done.");
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
