#include <meminfo/memory/memory_control.h>

#include <control_generated.h>
#include <spdlog/spdlog.h>

#include <utility>

namespace meminfo {
namespace memory {

MemoryControlSocket::MemoryControlSocket(const SlabAllocator* allocator, std::string socket_name)
    : allocator_(allocator), socket_name_(std::move(socket_name)) {
    ipc_ = platform::create_local_ipc();
}

MemoryControlSocket::~MemoryControlSocket() {
    stop();
}

void MemoryControlSocket::start() {
    if (started_ || socket_name_.empty()) return;
    if (!ipc_) ipc_ = platform::create_local_ipc(); // after a stop()
    ipc_->listen(socket_name_, [this](const std::vector<uint8_t>& req, std::vector<uint8_t>& resp) {
        handle_request(req, resp);
    });
    started_ = true;
    spdlog::info("memoryd control socket listening on {}", socket_name_);
}

void MemoryControlSocket::stop() {
    // ILocalIpc tears the listener down in its own destructor; releasing it
    // here is what actually stops the accept loop and unlinks the socket.
    if (!started_) return;
    ipc_.reset();
    started_ = false;
}

void MemoryControlSocket::handle_request(const std::vector<uint8_t>& data, std::vector<uint8_t>& resp) {
    if (data.size() < 4) return;

    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!meminfo::control::VerifySizePrefixedControlRequestBuffer(verifier)) {
        return;
    }

    const auto* req = meminfo::control::GetSizePrefixedControlRequest(data.data());

    bool success = false;
    std::string message = "Unsupported command";
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;

    if (req->command() == meminfo::control::ControlCommand_GET_MEMORY_STATS && allocator_) {
        const size_t page_size = allocator_->page_size();
        total_bytes = static_cast<uint64_t>(allocator_->total_pages()) * page_size;
        free_bytes = static_cast<uint64_t>(allocator_->free_pages_count()) * page_size;
        success = true;
        message = "OK";
    }

    flatbuffers::FlatBufferBuilder builder;
    auto fb_msg = builder.CreateString(message);

    meminfo::control::ControlResponseBuilder crb(builder);
    crb.add_request_id(req->request_id());
    crb.add_success(success);
    crb.add_message(fb_msg);
    if (success) {
        crb.add_pool_total_bytes(total_bytes);
        crb.add_pool_free_bytes(free_bytes);
        crb.add_pool_used_bytes(total_bytes - free_bytes);
    }
    builder.FinishSizePrefixed(crb.Finish());

    resp.assign(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
}

} // namespace memory
} // namespace meminfo
