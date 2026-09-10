#include <meminfo/client/client_api.h>
#include <meminfo/common/crc32c.h>
#include <meminfo/common/protocol_version.h>
#include <meminfo/platform/IMemoryMonitor.h>
#include <meminfo/platform/ILocalIpc.h>
#include <spdlog/spdlog.h>
#include <queue>
#include <algorithm>
#include <chrono>

namespace meminfo {
namespace client {

MemoryClient::MemoryClient(size_t max_local_bytes, const std::string& discovery_socket, const std::string& test_ip, int test_port)
    : discovery_socket_(discovery_socket), cache_(max_local_bytes) {
    
    memory_monitor_ = platform::create_memory_monitor();
    memory_monitor_->set_pressure_callback([this]() {
        spdlog::warn("MemoryClient: High memory pressure detected by OS! Aggressively evicting...");
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
                uv_buf_t buf = uv_buf_init(reinterpret_cast<char*>(wr->buf_data.data()),
                                           static_cast<unsigned int>(wr->buf_data.size()));

                // Must be set before uv_write: the write can complete inline,
                // running the callback before uv_write returns.
                wr->req.data = wr;

                int wr_rc = uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(msg.peer->socket),
                                     &buf, 1, [](uv_write_t* req, int) {
                    delete static_cast<WriteReq*>(req->data);
                });
                if (wr_rc != 0) {
                    // The callback never runs when uv_write fails outright.
                    delete wr;
                }
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
        auto* p = static_cast<MemoryClient::RemotePeer*>(req->data);
        if (status == 0) {
            p->connected = true;
            uv_read_start(reinterpret_cast<uv_stream_t*>(p->socket), 
                [](uv_handle_t*, size_t suggested, uv_buf_t* b) {
                    b->base = new char[suggested];
                    // uv_buf_t::len is size_t on Unix but a 32-bit ULONG on Windows, so this
                    // assignment narrows there; make the conversion explicit.
                    b->len = static_cast<decltype(b->len)>(suggested);
                },
                MemoryClient::on_peer_read);
        }
        delete req;
    });
    
    peers_.push_back(std::move(peer));
}

MemoryClient::~MemoryClient() {
    running_ = false;
    uv_async_send(&stop_async_);
    if (network_thread_.joinable()) {
        network_thread_.join();
    }

    // The stop callback calls uv_stop() straight after uv_close(), so uv_run
    // returns before the close callbacks have executed. Drain them here,
    // otherwise uv_loop_close() fails with EBUSY and the handles leak.
    uv_run(&loop_, UV_RUN_DEFAULT);

    int rc = uv_loop_close(&loop_);
    if (rc != 0) {
        spdlog::warn("MemoryClient loop did not close cleanly: {}", uv_strerror(rc));
    }
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

void MemoryClient::refresh_peer_capacity() {
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
                auto now = std::chrono::steady_clock::now();
                for (const auto* p : *resp->peers()) {
                    if (p->memory_port() > 0 && p->address()) {
                        std::string addr = p->address()->str();
                        uint16_t port = p->memory_port();
                        
                        // Find matching peer
                        for (auto& peer : peers_) {
                            if (peer->ip == addr && peer->port == port) {
                                peer->free_ram_bytes = p->free_ram_bytes();
                                peer->free_vram_bytes = p->free_vram_bytes();
                                peer->capacity_known = true;
                                peer->last_capacity_update = now;
                                break;
                            }
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("Failed to refresh peer capacity: {}", e.what());
    }
}

MemoryClient::RemotePeer* MemoryClient::select_best_peer(size_t size_needed) {
    // Refresh capacity if TTL expired
    auto now = std::chrono::steady_clock::now();
    bool need_refresh = true;
    for (const auto& peer : peers_) {
        if (peer->connected && peer->capacity_known &&
            now - peer->last_capacity_update < PEER_CAPACITY_TTL) {
            need_refresh = false;
            break;
        }
    }
    if (need_refresh) {
        refresh_peer_capacity();
    }
    
    // Find the connected peer with the most free RAM that can fit the
    // allocation. A peer whose capacity discovery has not reported is a
    // candidate of last resort rather than an exclusion.
    MemoryClient::RemotePeer* best_peer = nullptr;
    uint64_t best_free = 0;
    MemoryClient::RemotePeer* unknown_peer = nullptr;

    for (auto& peer : peers_) {
        if (!peer->connected) continue;

        if (!peer->capacity_known) {
            if (!unknown_peer) unknown_peer = peer.get();
        } else if (peer->free_ram_bytes >= size_needed && peer->free_ram_bytes > best_free) {
            best_free = peer->free_ram_bytes;
            best_peer = peer.get();
        }
    }

    return best_peer ? best_peer : unknown_peer;
}

std::vector<uint8_t> MemoryClient::sync_remote_call(MemoryClient::RemotePeer* peer, const uint8_t* payload, size_t size, uint64_t request_id) {
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

void MemoryClient::allocate_remote(handle_t handle, size_t size) {
    // Try to allocate on best peer, with fallback to other peers
    std::vector<MemoryClient::RemotePeer*> candidates;
    
    // Get all connected peers sorted by free RAM (descending)
    for (auto& peer : peers_) {
        if (peer->connected) {
            candidates.push_back(peer.get());
        }
    }
    
    // Most free RAM first; peers whose capacity discovery has not reported sort
    // last but are still tried, since "unknown" is not the same as "full".
    std::sort(candidates.begin(), candidates.end(), [](MemoryClient::RemotePeer* a, MemoryClient::RemotePeer* b) {
        if (a->capacity_known != b->capacity_known) return a->capacity_known;
        return a->free_ram_bytes > b->free_ram_bytes;
    });
    
    RemoteAllocation alloc;
    alloc.size = size;
    alloc.is_striped = false;
    
    bool allocated = false;
    
    // First try: single peer allocation
    for (MemoryClient::RemotePeer* peer : candidates) {
        if (peer->may_fit(size)) {
            uint64_t req_id = next_request_id_++;
            flatbuffers::FlatBufferBuilder builder;
            meminfo::memory::MemoryRequestBuilder mrb(builder);
            mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
            mrb.add_request_id(req_id);
            mrb.add_op(meminfo::memory::OpCode_ALLOC);
            mrb.add_size(size);
            builder.FinishSizePrefixed(mrb.Finish());
            
            try {
                auto resp_data = sync_remote_call(peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
                if (!resp_data.empty()) {
                    flatbuffers::Verifier verifier(resp_data.data(), resp_data.size());
                    if (verifier.VerifySizePrefixedBuffer<meminfo::memory::MemoryResponse>(nullptr)) {
                        const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::memory::MemoryResponse>(resp_data.data());
                        if (resp->status() == meminfo::memory::StatusCode_OK) {
                            alloc.remote_handle = resp->handle();
                            alloc.peer = peer;
                            allocated = true;
                            spdlog::info("Allocated {} bytes on peer {}:{} (remote handle {})", size, peer->ip, peer->port, resp->handle());
                            break;
                        }
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to allocate on peer {}:{}: {}", peer->ip, peer->port, e.what());
                // Continue to next peer
            }
        }
    }
    
    // Second try: striping across multiple peers if single allocation failed
    if (!allocated && candidates.size() > 1) {
        spdlog::info("Single peer allocation failed, attempting striping across {} peers", candidates.size());
        
        size_t remaining = size;
        size_t current_offset = 0;
        
        for (MemoryClient::RemotePeer* peer : candidates) {
            if (remaining == 0) break;
            
            // An unknown-capacity peer is offered the whole remainder; the
            // daemon rejects what it cannot hold.
            size_t chunk_size = peer->capacity_known
                ? std::min<size_t>(remaining, static_cast<size_t>(peer->free_ram_bytes))
                : remaining;
            if (chunk_size == 0) continue;
            
            uint64_t req_id = next_request_id_++;
            flatbuffers::FlatBufferBuilder builder;
            meminfo::memory::MemoryRequestBuilder mrb(builder);
            mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
            mrb.add_request_id(req_id);
            mrb.add_op(meminfo::memory::OpCode_ALLOC);
            mrb.add_size(chunk_size);
            builder.FinishSizePrefixed(mrb.Finish());
            
            try {
                auto resp_data = sync_remote_call(peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
                if (!resp_data.empty()) {
                    flatbuffers::Verifier verifier(resp_data.data(), resp_data.size());
                    if (verifier.VerifySizePrefixedBuffer<meminfo::memory::MemoryResponse>(nullptr)) {
                        const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::memory::MemoryResponse>(resp_data.data());
                        if (resp->status() == meminfo::memory::StatusCode_OK) {
                            RemoteAllocation::Stripe stripe;
                            stripe.remote_handle = resp->handle();
                            stripe.offset = current_offset;
                            stripe.length = chunk_size;
                            stripe.peer = peer;
                            alloc.stripes.push_back(std::move(stripe));
                            current_offset += chunk_size;
                            remaining -= chunk_size;
                            spdlog::info("Stripe allocated: {} bytes on peer {}:{} (remote handle {})", chunk_size, peer->ip, peer->port, resp->handle());
                        }
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to allocate stripe on peer {}:{}: {}", peer->ip, peer->port, e.what());
            }
        }
        
        if (remaining == 0 && !alloc.stripes.empty()) {
            alloc.is_striped = true;
            allocated = true;
            spdlog::info("Successfully striped {} bytes across {} peers", size, alloc.stripes.size());
        } else if (!alloc.stripes.empty()) {
            // Partial striping: release what was already reserved instead of
            // stranding it on the remote peers.
            spdlog::warn("Striping incomplete ({} bytes unplaced); releasing {} partial stripe(s)",
                         remaining, alloc.stripes.size());
            RemoteAllocation partial;
            partial.is_striped = true;
            partial.stripes = std::move(alloc.stripes);
            free_remote(partial);
            alloc.stripes.clear();
        }
    }
    
    if (!allocated) {
        throw std::runtime_error("Failed to allocate on any peer: out of memory on all peers");
    }
    
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        remote_handles_[handle] = std::move(alloc);
    }
}

void MemoryClient::free_remote(const RemoteAllocation& alloc) {
    if (alloc.is_striped) {
        for (const auto& stripe : alloc.stripes) {
            if (!stripe.peer || !stripe.peer->connected) continue;
            
            uint64_t req_id = next_request_id_++;
            flatbuffers::FlatBufferBuilder builder;
            meminfo::memory::MemoryRequestBuilder mrb(builder);
            mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
            mrb.add_request_id(req_id);
            mrb.add_op(meminfo::memory::OpCode_FREE);
            mrb.add_handle(stripe.remote_handle);
            builder.FinishSizePrefixed(mrb.Finish());
            
            try {
                sync_remote_call(stripe.peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
            } catch (const std::exception& e) {
                spdlog::warn("Failed to free stripe on peer: {}", e.what());
            }
        }
    } else {
        if (!alloc.peer || !alloc.peer->connected) return;
        
        uint64_t req_id = next_request_id_++;
        flatbuffers::FlatBufferBuilder builder;
        meminfo::memory::MemoryRequestBuilder mrb(builder);
        mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
        mrb.add_request_id(req_id);
        mrb.add_op(meminfo::memory::OpCode_FREE);
        mrb.add_handle(alloc.remote_handle);
        builder.FinishSizePrefixed(mrb.Finish());
        
        try {
            sync_remote_call(alloc.peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
        } catch (const std::exception& e) {
            spdlog::warn("Failed to free remote allocation: {}", e.what());
        }
    }
}

void MemoryClient::write_remote(const RemoteAllocation& alloc, size_t offset, const uint8_t* data, size_t size) {
    if (alloc.is_striped) {
        size_t data_offset = 0;
        while (data_offset < size) {
            for (const auto& stripe : alloc.stripes) {
                if (data_offset >= size) break;
                if (offset + data_offset < stripe.offset || offset + data_offset >= stripe.offset + stripe.length) {
                    continue; // This stripe doesn't cover this range
                }
                
                size_t stripe_write_offset = (offset + data_offset) - stripe.offset;
                size_t stripe_write_size = std::min<size_t>(size - data_offset, stripe.length - stripe_write_offset);
                
                uint64_t req_id = next_request_id_++;
                flatbuffers::FlatBufferBuilder builder;
                auto fb_data = builder.CreateVector(data + data_offset, stripe_write_size);
                meminfo::memory::MemoryRequestBuilder mrb(builder);
                mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
                mrb.add_request_id(req_id);
                mrb.add_op(meminfo::memory::OpCode_WRITE);
                mrb.add_handle(stripe.remote_handle);
                mrb.add_offset(stripe_write_offset);
                mrb.add_data(fb_data);
                mrb.add_checksum(crc32c(data + data_offset, stripe_write_size));
                builder.FinishSizePrefixed(mrb.Finish());
                
                try {
                    sync_remote_call(stripe.peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
                } catch (const std::exception& e) {
                    throw std::runtime_error("Failed to write stripe: " + std::string(e.what()));
                }
                
                data_offset += stripe_write_size;
            }
        }
    } else {
        if (!alloc.peer || !alloc.peer->connected) throw std::runtime_error("Peer not connected");
        
        uint64_t req_id = next_request_id_++;
        flatbuffers::FlatBufferBuilder builder;
        auto fb_data = builder.CreateVector(data, size);
        meminfo::memory::MemoryRequestBuilder mrb(builder);
        mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
        mrb.add_request_id(req_id);
        mrb.add_op(meminfo::memory::OpCode_WRITE);
        mrb.add_handle(alloc.remote_handle);
        mrb.add_offset(offset);
        mrb.add_data(fb_data);
        mrb.add_checksum(crc32c(data, size));
        builder.FinishSizePrefixed(mrb.Finish());
        
        sync_remote_call(alloc.peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
    }
}

std::vector<uint8_t> MemoryClient::read_remote(const RemoteAllocation& alloc, size_t offset, size_t size) {
    std::vector<uint8_t> result(size);
    
    if (alloc.is_striped) {
        size_t result_offset = 0;
        while (result_offset < size) {
            for (const auto& stripe : alloc.stripes) {
                if (result_offset >= size) break;
                if (offset + result_offset < stripe.offset || offset + result_offset >= stripe.offset + stripe.length) {
                    continue; // This stripe doesn't cover this range
                }
                
                size_t stripe_read_offset = (offset + result_offset) - stripe.offset;
                size_t stripe_read_size = std::min(size - result_offset, stripe.length - stripe_read_offset);
                
                uint64_t req_id = next_request_id_++;
                flatbuffers::FlatBufferBuilder builder;
                meminfo::memory::MemoryRequestBuilder mrb(builder);
                mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
                mrb.add_request_id(req_id);
                mrb.add_op(meminfo::memory::OpCode_READ);
                mrb.add_handle(stripe.remote_handle);
                mrb.add_offset(stripe_read_offset);
                mrb.add_size(stripe_read_size);
                builder.FinishSizePrefixed(mrb.Finish());
                
                try {
                    auto resp_data = sync_remote_call(stripe.peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
                    if (resp_data.empty()) throw std::runtime_error("Empty response");
                    
                    flatbuffers::Verifier verifier(resp_data.data(), resp_data.size());
                    if (!verifier.VerifySizePrefixedBuffer<meminfo::memory::MemoryResponse>(nullptr)) {
                        throw std::runtime_error("Invalid MemoryResponse buffer");
                    }
                    const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::memory::MemoryResponse>(resp_data.data());
                    
                    if (resp->status() != meminfo::memory::StatusCode_OK || !resp->data()) {
                        throw std::runtime_error("Failed to read from remote peer");
                    }
                    
                    std::vector<uint8_t> stripe_data(resp->data()->begin(), resp->data()->end());
                    std::copy(stripe_data.begin(), stripe_data.end(), result.begin() + result_offset);
                    result_offset += stripe_read_size;
                    
                    // Free from remote after read
                    req_id = next_request_id_++;
                    builder.Clear();
                    meminfo::memory::MemoryRequestBuilder mrb2(builder);
                    mrb2.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
                    mrb2.add_request_id(req_id);
                    mrb2.add_op(meminfo::memory::OpCode_FREE);
                    mrb2.add_handle(stripe.remote_handle);
                    builder.FinishSizePrefixed(mrb2.Finish());
                    sync_remote_call(stripe.peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
                } catch (const std::exception& e) {
                    throw std::runtime_error("Failed to read stripe: " + std::string(e.what()));
                }
            }
        }
    } else {
        if (!alloc.peer || !alloc.peer->connected) throw std::runtime_error("Peer not connected");
        
        uint64_t req_id = next_request_id_++;
        flatbuffers::FlatBufferBuilder builder;
        meminfo::memory::MemoryRequestBuilder mrb(builder);
        mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
        mrb.add_request_id(req_id);
        mrb.add_op(meminfo::memory::OpCode_READ);
        mrb.add_handle(alloc.remote_handle);
        mrb.add_offset(offset);
        mrb.add_size(size);
        builder.FinishSizePrefixed(mrb.Finish());
        
        auto resp_data = sync_remote_call(alloc.peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
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
        result = std::move(data);
        
        // Free from remote after read
        req_id = next_request_id_++;
        builder.Clear();
        meminfo::memory::MemoryRequestBuilder mrb2(builder);
        mrb2.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
        mrb2.add_request_id(req_id);
        mrb2.add_op(meminfo::memory::OpCode_FREE);
        mrb2.add_handle(alloc.remote_handle);
        builder.FinishSizePrefixed(mrb2.Finish());
        sync_remote_call(alloc.peer, builder.GetBufferPointer(), builder.GetSize(), req_id);
    }
    
    return result;
}

void MemoryClient::free(handle_t handle) {
    if (cache_.remove(handle)) {
        return; // was purely local
    }
    
    std::lock_guard<std::mutex> lock(remote_mutex_);
    auto it = remote_handles_.find(handle);
    if (it != remote_handles_.end()) {
        free_remote(it->second);
        remote_handles_.erase(it);
    }
}

std::vector<uint8_t> MemoryClient::read(handle_t handle, size_t offset, size_t size) {
    auto local_opt = cache_.get(handle);
    if (local_opt) {
        if (offset > local_opt->size() || size > local_opt->size() - offset) throw std::out_of_range("Read out of bounds");
        return std::vector<uint8_t>(local_opt->begin() + offset, local_opt->begin() + offset + size);
    }
    
    load_to_local(handle); // Throws if invalid handle
    
    local_opt = cache_.get(handle);
    if (!local_opt) throw std::runtime_error("Failed to load block from remote");
    if (offset > local_opt->size() || size > local_opt->size() - offset) throw std::out_of_range("Read out of bounds");
    return std::vector<uint8_t>(local_opt->begin() + offset, local_opt->begin() + offset + size);
}

void MemoryClient::write(handle_t handle, size_t offset, const std::vector<uint8_t>& data) {
    auto local_opt = cache_.get(handle);
    if (local_opt) {
        if (offset > local_opt->size() || data.size() > local_opt->size() - offset) throw std::out_of_range("Write out of bounds");
        std::copy(data.begin(), data.end(), local_opt->begin() + offset);
        cache_.put(handle, std::move(*local_opt), true);
        return;
    }
    
    load_to_local(handle);
    
    local_opt = cache_.get(handle);
    if (!local_opt) throw std::runtime_error("Failed to load block from remote");
    if (offset > local_opt->size() || data.size() > local_opt->size() - offset) throw std::out_of_range("Write out of bounds");
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

        // evict_one() has already removed the block from the cache, so if the
        // push to a peer fails the only copy of the data is the one in hand.
        // Restore it before propagating the error.
        try {
            allocate_remote(evicted->handle, evicted->data.size());

            RemoteAllocation alloc_copy;
            {
                std::lock_guard<std::mutex> lock(remote_mutex_);
                auto it = remote_handles_.find(evicted->handle);
                if (it == remote_handles_.end()) {
                    throw std::runtime_error("Remote allocation vanished after allocate");
                }
                alloc_copy = it->second;
            }

            // Not under remote_mutex_: write_remote blocks on the network and
            // would stall every other handle operation.
            write_remote(alloc_copy, 0, evicted->data.data(), evicted->data.size());
        } catch (...) {
            // Roll back: drop any remote reservation made for this handle, then
            // return the block to the cache so the data is not lost.
            RemoteAllocation orphan;
            bool has_orphan = false;
            {
                std::lock_guard<std::mutex> lock(remote_mutex_);
                auto it = remote_handles_.find(evicted->handle);
                if (it != remote_handles_.end()) {
                    orphan = std::move(it->second);
                    remote_handles_.erase(it);
                    has_orphan = true;
                }
            }
            if (has_orphan) {
                try {
                    free_remote(orphan);
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to release remote allocation during rollback: {}", e.what());
                }
            }

            cache_.put(evicted->handle, std::move(evicted->data), evicted->dirty);
            throw;
        }

    }
}

void MemoryClient::load_to_local(handle_t handle) {
    RemoteAllocation alloc;
    size_t size = 0;
    
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        auto it = remote_handles_.find(handle);
        if (it == remote_handles_.end()) {
            throw std::invalid_argument("Invalid handle or block not found");
        }
        alloc = it->second;
        size = it->second.size;
    }
    
    evict_if_needed(size);
    
    // Read data from remote
    std::vector<uint8_t> data = read_remote(alloc, 0, size);
    
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        remote_handles_.erase(handle);
    }
    
    cache_.put(handle, std::move(data), false); // Data is loaded, clean state
}

void MemoryClient::on_peer_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* peer = static_cast<MemoryClient::RemotePeer*>(stream->data);
    
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
    } else if (nread < 0) {
        // EOF or error: the peer will not answer anything still in flight.
        if (nread != UV_EOF) {
            spdlog::warn("Peer {}:{} read error: {}", peer->ip, peer->port, uv_strerror(static_cast<int>(nread)));
        }
        peer->connected = false;
        fail_pending_requests(peer->client);

        // Must free the handle here. A nullptr callback would leak it, and the
        // shutdown walk skips handles that are already closing, so nothing else
        // would ever reclaim it.
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(stream))) {
            peer->socket = nullptr;
            uv_close(reinterpret_cast<uv_handle_t*>(stream), [](uv_handle_t* h) {
                delete reinterpret_cast<uv_tcp_t*>(h);
            });
        }
    }
    if (buf->base) delete[] buf->base;
}

void MemoryClient::fail_pending_requests(MemoryClient* client) {
    if (!client) return;

    std::unordered_map<uint64_t, std::shared_ptr<RequestContext>> pending;
    {
        std::lock_guard<std::mutex> lock(client->requests_mutex_);
        pending.swap(client->pending_requests_);
    }

    // An empty buffer is the caller-visible signal for "no usable response".
    for (auto& entry : pending) {
        if (entry.second) {
            entry.second->promise.set_value(std::vector<uint8_t>());
        }
    }
}

} // namespace client
} // namespace meminfo
