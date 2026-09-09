#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

namespace meminfo {
namespace platform {

class ILocalIpc {
public:
    // Callback signature: takes a request buffer and populates the response buffer.
    using MessageHandler = std::function<void(const std::vector<uint8_t>& req, std::vector<uint8_t>& resp)>;
    
    // Listen for incoming requests on the given name (UDS path or Named Pipe name).
    virtual void listen(const std::string& name, MessageHandler cb) = 0;
    
    // Connect to the given name, send a request, and return the response.
    virtual std::vector<uint8_t> send_request(const std::string& name, const std::vector<uint8_t>& req) = 0;
    
    virtual ~ILocalIpc() = default;
};

// Factory
std::unique_ptr<ILocalIpc> create_local_ipc();

} // namespace platform
} // namespace meminfo
