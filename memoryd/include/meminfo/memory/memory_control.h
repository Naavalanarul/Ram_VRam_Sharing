#pragma once
#include <meminfo/memory/slab_allocator.h>
#include <meminfo/platform/ILocalIpc.h>
#include <memory>
#include <string>
#include <vector>

namespace meminfo {
namespace memory {

// Local control surface for memoryd, mirroring discoveryd's ControlSocket.
//
// It exists so discoveryd can announce the pool this daemon actually allocates
// out of. Before this, discoveryd announced the OS's free-RAM figure, which is
// unrelated to total_reserved_bytes: a node whose slab pool was full still
// advertised gigabytes of capacity, and clients kept choosing it only to be
// answered OUT_OF_MEMORY.
//
// The transport is ILocalIpc (Unix domain socket on POSIX, named pipe on
// Windows), so it is reachable only from the same machine.
class MemoryControlSocket {
public:
    MemoryControlSocket(const SlabAllocator* allocator, std::string socket_name);
    ~MemoryControlSocket();

    MemoryControlSocket(const MemoryControlSocket&) = delete;
    MemoryControlSocket& operator=(const MemoryControlSocket&) = delete;

    void start();
    void stop();

    const std::string& socket_name() const { return socket_name_; }

private:
    void handle_request(const std::vector<uint8_t>& req, std::vector<uint8_t>& resp);

    const SlabAllocator* allocator_;
    std::string socket_name_;
    std::unique_ptr<platform::ILocalIpc> ipc_;
    bool started_ = false;
};

} // namespace memory
} // namespace meminfo
