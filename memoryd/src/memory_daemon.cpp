#include <meminfo/memory/memory_daemon.h>
#include <meminfo/memory/client_session.h>
#include <spdlog/spdlog.h>

namespace meminfo {
namespace memory {

MemoryDaemon::MemoryDaemon(const Config& config) 
    : config_(config) {
    
    uv_loop_init(&loop_);
    
    uv_async_init(&loop_, &stop_async_, [](uv_async_t* handle) {
        auto* self = static_cast<MemoryDaemon*>(handle->data);
        self->close_sockets();
        uv_stop(&self->loop_);
        self->is_running_ = false;
    });
    stop_async_.data = this;
    
    size_t total_size = config_.get<size_t>("memory", "total_reserved_bytes", 1024 * 1024 * 256); // Default 256MB
    size_t page_size = config_.get<size_t>("memory", "page_size_bytes", 4096);
    
    allocator_ = std::make_unique<SlabAllocator>(total_size, page_size);
    page_tracker_ = std::make_unique<PageTracker>(allocator_.get());
    
    uv_tcp_init(&loop_, &server_socket_);
    server_socket_.data = this;
    
    sig_handler_ = std::make_unique<SignalHandler>(&loop_, [this]() {
        spdlog::info("Received termination signal");
        this->stop();
    });
}

// Closes the listening socket and every accepted client socket.
//
// Must run on the loop thread. Client sessions own themselves and are deleted
// by their own close callback; the listening socket's data points at this
// daemon, so it must not go through that path.
void MemoryDaemon::close_sockets() {
    uv_walk(&loop_, [](uv_handle_t* handle, void* arg) {
        auto* self = static_cast<MemoryDaemon*>(arg);
        if (uv_is_closing(handle) || handle->type != UV_TCP) return;

        if (handle == reinterpret_cast<uv_handle_t*>(&self->server_socket_)) {
            uv_close(handle, nullptr);
        } else {
            // Accepted connection: hand it to the session's own teardown so the
            // ClientSession is destroyed and its allocations are released.
            uv_close(handle, ClientSession::on_close_handle);
        }
    }, this);
}

MemoryDaemon::~MemoryDaemon() {
    // Close everything here rather than signalling stop_async_: the loop is no
    // longer running by this point, so an async callback would never fire, and
    // the still-open listening socket would keep the drain below blocked
    // forever.
    close_sockets();

    sig_handler_.reset(); // Close signal handles

    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&stop_async_))) {
        uv_close(reinterpret_cast<uv_handle_t*>(&stop_async_), nullptr);
    }

    is_running_ = false;

    // Every handle is now closing, so this drains their close callbacks and
    // returns instead of blocking on live ones.
    uv_run(&loop_, UV_RUN_DEFAULT);
    uv_loop_close(&loop_);
}

void MemoryDaemon::run() {
    if (is_running_) return;
    
    std::string listen_addr = config_.get<std::string>("memory", "listen_address", "0.0.0.0");
    int port = config_.get<int>("memory", "port", 9200);
    
    struct sockaddr_in bind_addr;
    uv_ip4_addr(listen_addr.c_str(), port, &bind_addr);
    
    int r = uv_tcp_bind(&server_socket_, reinterpret_cast<const struct sockaddr*>(&bind_addr), 0);
    if (r < 0) {
        spdlog::error("Bind error: {}", uv_strerror(r));
        throw std::runtime_error(std::string("Bind error: ") + uv_strerror(r));
    }
    
    // Get the actual port if port 0 was specified
    if (port == 0) {
        struct sockaddr_in actual_addr;
        int namelen = sizeof(actual_addr);
        uv_tcp_getsockname(&server_socket_, reinterpret_cast<struct sockaddr*>(&actual_addr), &namelen);
        listen_port_ = ntohs(actual_addr.sin_port);
    } else {
        listen_port_ = port;
    }
    
    r = uv_listen(reinterpret_cast<uv_stream_t*>(&server_socket_), 128, on_connection);
    if (r < 0) {
        spdlog::error("Listen error: {}", uv_strerror(r));
        throw std::runtime_error(std::string("Listen error: ") + uv_strerror(r));
    }
    
    spdlog::info("memoryd listening on {}:{}", listen_addr, listen_port_);
    is_running_ = true;
    
    uv_run(&loop_, UV_RUN_DEFAULT);
}

void MemoryDaemon::stop() {
    // No is_running_ guard: stop() may legitimately arrive before run() has
    // flipped the flag, and dropping it there would strand the daemon.
    // uv_async_send is safe from any thread once the handle is initialised.
    if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&stop_async_))) return;
    spdlog::info("Stopping memoryd...");
    uv_async_send(&stop_async_);
}

void MemoryDaemon::on_connection(uv_stream_t* server, int status) {
    if (status < 0) {
        spdlog::error("New connection error: {}", uv_strerror(status));
        return;
    }
    
    auto* self = static_cast<MemoryDaemon*>(server->data);
    
    // ClientSession self-manages its memory after creation (deleted in on_close)
    new ClientSession(&self->loop_, server, self->page_tracker_.get(), self->next_session_id_++);
}

} // namespace memory
} // namespace meminfo
