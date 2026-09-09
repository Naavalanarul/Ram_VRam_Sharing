#include <meminfo/client/client_api.h>
#include <meminfo/common/crc32c.h>
#include <meminfo/common/protocol_version.h>
#include <meminfo/platform/IMemoryMonitor.h>
#include <meminfo/platform/ILocalIpc.h>
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <queue>

namespace meminfo {
namespace client {

MemoryClient::MemoryClient(size_t max_local_bytes, const std::string& discovery_socket, const std::string& test_ip, int test_port)
    : discovery_socket_(discovery_socket), cache_(max_local_bytes) {
    
    memory_monitor_ = platform::create_memory_monitor();
    memory_monitor_->set_pressure_callback([this]() {
        spdlog::warn("MemoryClient: High memory pressure detected by OS! Aggressively evicting...");
        // On pressure, try to evict half our cache, or at least 1MB
        size_t to_evict = std::max(cache_.current_size() / 2, (size_t)1048576);
        this->evict_if_needed(to_evict);
    });

    uv_loop_init(&loop_);
    
    uv_async_init(&loop_, &stop_async_, [](uv_async_t* handle) {
        auto* self = static_cast<MemoryClient*>(handle->data);
        uv_walk(&self->loop_, [](uv_handle_t* h, void*) {
            if (!uv_is_closing(h)) {
                uv_close(h, [](uv_handle_t* ch) {
                    if (ch->type == UV_TCP) delete reinterpret_cast<uv_tcp_t*>(ch);
                });
            }
        }, nullptr);
        uv_stop(&self->loop_);
    });
    stop_async_.data = this;
    
    uv_async_init(&loop_, &wakeup_async_, [](uv_async_t* handle) {
        auto* self = static_cast<MemoryClient*>(handle->data);
        std::queue<OutboundMessage> batch;
        {
            std::lock_guard<std::mutex> lock(self->queue_mutex_);
            std::swap(batch, self->outbound_queue_);
        }
        
        while (!batch.empty()) {
            auto msg = std::move(batch.front());
            batch.pop();
            
            if (msg.peer && msg.peer->connected) {
                struct WriteReq {
                    uv_write_t req;
                    std::vector<uint8_t> buf_data;
                };
                
                auto* wr = new WriteReq;
                wr->buf_data = std::move(msg.payload);
                uv_buf_t buf = uv_buf_init(reinterpret_cast<char*>(wr->buf_data.data()), wr->buf_data.size());
                
                uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(msg.peer->socket), &buf, 1, [](uv_write_t* req, int) {
                    delete static_cast<WriteReq*>(req->data);
                });
                wr->req.data = wr;
            }
        }
    });
    wakeup_async_.data = this;
    
    if (!test_ip.empty() && test_port > 0) {
        add_peer_and_connect(test_ip, test_port);
    } else {
        connect_to_peers();
    }
    
    network_thread_ = std::thread(&MemoryClient::network_thread_main, this);
}

void MemoryClient::add_peer_and_connect(const std::string& ip, int port) {
    auto peer = std::make_unique<RemotePeer>();
    peer->client = this;
    peer->ip = ip;
    peer->port = port;
    
    peer->socket = new uv_tcp_t;
    uv_tcp_init(&loop_, peer->socket);
    peer->socket->data = peer.get();
    
    struct sockaddr_in dest;
    uv_ip4_addr(peer->ip.c_str(), peer->port, &dest);
    
    uv_connect_t* conn = new uv_connect_t;
    conn->data = peer.get();
    uv_tcp_connect(conn, peer->socket, reinterpret_cast<const struct sockaddr*>(&dest), [](uv_connect_t* req, int status) {
        auto* p = static_cast<RemotePeer*>(req->data);
        if (status == 0) {
            p->connected = true;
            uv_read_start(reinterpret_cast<uv_stream_t*>(p->socket), 
                [](uv_handle_t*, size_t suggested, uv_buf_t* b) {
                    b->base = new char[suggested];
                    b->len = suggested;
                },
                MemoryClient::on_peer_read);
        }
        delete req;
    });
    
    if (!current_peer_) current_peer_ = peer.get();
    peers_.push_back(std::move(peer));
}

MemoryClient::~MemoryClient() {
    running_ = false;
    uv_async_send(&stop_async_);
    if (network_thread_.joinable()) {
        network_thread_.join();
    }
    uv_loop_close(&loop_);
}

void MemoryClient::network_thread_main() {
    uv_run(&loop_, UV_RUN_DEFAULT);
}

void MemoryClient::connect_to_peers() {
    auto ipc = platform::create_local_ipc();
    
    flatbuffers::FlatBufferBuilder builder;
    meminfo::control::ControlRequestBuilder crb(builder);
    crb.add_command(meminfo::control::ControlCommand_LIST_PEERS);
    builder.FinishSizePrefixed(crb.Finish());
    
    std::vector<uint8_t> req(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
    
    try {
        auto resp_buf = ipc->send_request(discovery_socket_, req);
        if (!resp_buf.empty()) {
            const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::control::ControlResponse>(resp_buf.data());
            if (resp && resp->peers()) {
                for (const auto* p : *resp->peers()) {
                    if (p->memory_port() > 0 && p->address()) {
                        add_peer_and_connect(p->address()->str(), p->memory_port());
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("Failed to fetch peers: {}", e.what());
    }
}

void MemoryClient::on_peer_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* peer = static_cast<RemotePeer*>(stream->data);
    
    if (nread > 0) {
        peer->read_buffer.insert(peer->read_buffer.end(), buf->base, buf->base + nread);
        
        while (peer->read_buffer.size() >= 4) {
            uint32_t msg_size = flatbuffers::GetPrefixedSize(peer->read_buffer.data());
            // Cap msg_size to prevent overflow (Bug #7 fix)
            if (msg_size > 64 * 1024 * 1024) {
                uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
                return;
            }
            if (peer->read_buffer.size() - 4 >= msg_size) {
                flatbuffers::Verifier verifier(peer->read_buffer.data(), msg_size + 4);
                if (!verifier.VerifySizePrefixedBuffer<meminfo::memory::MemoryResponse>(nullptr)) {
                    uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
                    return;
                }
                const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::memory::MemoryResponse>(peer->read_buffer.data());
                
                std::shared_ptr<RequestContext> req_ctx;
                {
                    std::lock_guard<std::mutex> lock(peer->client->requests_mutex_);
                    auto it = peer->client->pending_requests_.find(resp->request_id());
                    if (it != peer->client->pending_requests_.end()) {
                        req_ctx = it->second;
                        peer->client->pending_requests_.erase(it);
                    }
                }
                
                if (req_ctx) {
                    std::vector<uint8_t> result;
                    // Append full response buffer for decoding by the caller
                    result.assign(peer->read_buffer.data(), peer->read_buffer.data() + msg_size + 4);
                    req_ctx->promise.set_value(std::move(result));
                }
                
                peer->read_buffer.erase(peer->read_buffer.begin(), peer->read_buffer.begin() + msg_size + 4);
            } else {
                break;
            }
        }
    }
    if (buf->base) delete[] buf->base;
}

std::vector<uint8_t> MemoryClient::sync_remote_call(RemotePeer* peer, const uint8_t* payload, size_t size, uint64_t request_id) {
    auto req_ctx = std::make_shared<RequestContext>();
    req_ctx->request_id = request_id;
    auto future = req_ctx->promise.get_future();
    
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        pending_requests_[request_id] = req_ctx;
    }
    
    OutboundMessage msg;
    msg.peer = peer;
    msg.payload.assign(payload, payload + size);
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        outbound_queue_.push(std::move(msg));
    }
    uv_async_send(&wakeup_async_);
    
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        throw std::runtime_error("Remote call timed out");
    }
    
    return future.get();
}

handle_t MemoryClient::allocate(size_t size) {
    evict_if_needed(size);
    std::vector<uint8_t> initial_data(size, 0);
    handle_t handle = next_local_handle_++;
    cache_.put(handle, std::move(initial_data), false);
    return handle;
}

void MemoryClient::free(handle_t handle) {
    if (cache_.remove(handle)) {
        return; // was purely local
    }
    
    std::lock_guard<std::mutex> lock(remote_mutex_);
    auto it = remote_handles_.find(handle);
    if (it != remote_handles_.end()) {
        uint64_t remote_handle = it->second.remote_handle;
        RemotePeer* peer = handle_to_peer_[handle];
        
        uint64_t req_id = next_request_id_++;
        flatbuffers::FlatBufferBuilder builder;
        meminfo::memory::MemoryRequestBuilder mrb(builder);
        mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
        mrb.add_request_id(req_id);
        mrb.add_op(meminfo::memory::OpCode_FREE);
        mrb.add_handle(remote_handle);
        builder.FinishSizePrefixed(mrb.Finish());
        
        sync_remote_call(peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
        
        remote_handles_.erase(it);
        handle_to_peer_.erase(handle);
    }
}

std::vector<uint8_t> MemoryClient::read(handle_t handle, size_t offset, size_t size) {
    auto local_opt = cache_.get(handle);
    if (local_opt) {
        if (offset + size > local_opt->size()) throw std::out_of_range("Read out of bounds");
        return std::vector<uint8_t>(local_opt->begin() + offset, local_opt->begin() + offset + size);
    }
    
    load_to_local(handle); // Throws if invalid handle
    
    local_opt = cache_.get(handle);
    if (!local_opt) throw std::runtime_error("Failed to load block from remote");
    if (offset + size > local_opt->size()) throw std::out_of_range("Read out of bounds");
    return std::vector<uint8_t>(local_opt->begin() + offset, local_opt->begin() + offset + size);
}

void MemoryClient::write(handle_t handle, size_t offset, const std::vector<uint8_t>& data) {
    auto local_opt = cache_.get(handle);
    if (local_opt) {
        if (offset + data.size() > local_opt->size()) throw std::out_of_range("Write out of bounds");
        std::copy(data.begin(), data.end(), local_opt->begin() + offset);
        cache_.put(handle, std::move(*local_opt), true);
        return;
    }
    
    load_to_local(handle);
    
    local_opt = cache_.get(handle);
    if (!local_opt) throw std::runtime_error("Failed to load block from remote");
    if (offset + data.size() > local_opt->size()) throw std::out_of_range("Write out of bounds");
    std::copy(data.begin(), data.end(), local_opt->begin() + offset);
    cache_.put(handle, std::move(*local_opt), true);
}

void MemoryClient::evict_if_needed(size_t size_needed) {
    auto stats = memory_monitor_->get_stats();
    // Force eviction if OS has < 100MB available
    bool pressure_evict = stats.available_bytes > 0 && stats.available_bytes < (100 * 1024 * 1024);
    
    while (cache_.needs_eviction(size_needed) || pressure_evict) {
        auto evicted = cache_.evict_one();
        if (!evicted) break; // cache is empty
        
        if (current_peer_) {
            uint64_t remote_handle = 0;
            
            // Allocate on remote
            uint64_t req_id = next_request_id_++;
            flatbuffers::FlatBufferBuilder builder;
            meminfo::memory::MemoryRequestBuilder mrb(builder);
            mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
            mrb.add_request_id(req_id);
            mrb.add_op(meminfo::memory::OpCode_ALLOC);
            mrb.add_size(evicted->data.size());
            builder.FinishSizePrefixed(mrb.Finish());
            
            auto resp_data = sync_remote_call(current_peer_, builder.GetBufferPointer(), builder.GetSize(), req_id);
            if (resp_data.empty()) throw std::runtime_error("Empty response");
            flatbuffers::Verifier verifier(resp_data.data(), resp_data.size());
            if (!verifier.VerifySizePrefixedBuffer<meminfo::memory::MemoryResponse>(nullptr)) {
                throw std::runtime_error("Invalid MemoryResponse buffer");
            }
            const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::memory::MemoryResponse>(resp_data.data());
            
            if (resp->status() == meminfo::memory::StatusCode_OK) {
                remote_handle = resp->handle();
                
                // Write data
                req_id = next_request_id_++;
                builder.Clear();
                auto fb_data = builder.CreateVector(evicted->data);
                meminfo::memory::MemoryRequestBuilder mrb2(builder);
                mrb2.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
                mrb2.add_request_id(req_id);
                mrb2.add_op(meminfo::memory::OpCode_WRITE);
                mrb2.add_handle(remote_handle);
                mrb2.add_offset(0);
                mrb2.add_data(fb_data);
                mrb2.add_checksum(crc32c(evicted->data.data(), evicted->data.size()));
                builder.FinishSizePrefixed(mrb2.Finish());
                
                sync_remote_call(current_peer_, builder.GetBufferPointer(), builder.GetSize(), req_id);
                
                std::lock_guard<std::mutex> lock(remote_mutex_);
                remote_handles_[evicted->handle] = {remote_handle, evicted->data.size()};
                handle_to_peer_[evicted->handle] = current_peer_;
            } else {
                spdlog::error("Failed to allocate on remote: {}", resp->message() ? resp->message()->str() : "unknown");
            }
        }
    }
}

void MemoryClient::load_to_local(handle_t handle) {
    uint64_t remote_handle = 0;
    size_t size = 0;
    RemotePeer* peer = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        auto it = remote_handles_.find(handle);
        if (it == remote_handles_.end()) {
            throw std::invalid_argument("Invalid handle or block not found");
        }
        remote_handle = it->second.remote_handle;
        size = it->second.size;
        peer = handle_to_peer_[handle];
    }
    
    evict_if_needed(size);
    
    uint64_t req_id = next_request_id_++;
    flatbuffers::FlatBufferBuilder builder;
    meminfo::memory::MemoryRequestBuilder mrb(builder);
    mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
    mrb.add_request_id(req_id);
    mrb.add_op(meminfo::memory::OpCode_READ);
    mrb.add_handle(remote_handle);
    mrb.add_offset(0);
    mrb.add_size(size);
    builder.FinishSizePrefixed(mrb.Finish());
    
    auto resp_data = sync_remote_call(peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
    if (resp_data.empty()) throw std::runtime_error("Empty response");
    flatbuffers::Verifier verifier(resp_data.data(), resp_data.size());
    if (!verifier.VerifySizePrefixedBuffer<meminfo::memory::MemoryResponse>(nullptr)) {
        throw std::runtime_error("Invalid MemoryResponse buffer");
    }
    const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::memory::MemoryResponse>(resp_data.data());
    
    if (resp->status() != meminfo::memory::StatusCode_OK || !resp->data()) {
        throw std::runtime_error("Failed to read from remote peer");
    }
    
    std::vector<uint8_t> data(resp->data()->begin(), resp->data()->end());
    
    // Free from remote
    req_id = next_request_id_++;
    builder.Clear();
    meminfo::memory::MemoryRequestBuilder mrb2(builder);
    mrb2.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
    mrb2.add_request_id(req_id);
    mrb2.add_op(meminfo::memory::OpCode_FREE);
    mrb2.add_handle(remote_handle);
    builder.FinishSizePrefixed(mrb2.Finish());
    sync_remote_call(peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
    
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        remote_handles_.erase(handle);
        handle_to_peer_.erase(handle);
    }
    
    cache_.put(handle, std::move(data), false); // Data is loaded, clean state
}
} // namespace client
} // namespace meminfo
