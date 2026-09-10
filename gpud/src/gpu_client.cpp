#include <meminfo/gpu/gpu_client.h>
#include <meminfo/common/protocol_version.h>
#include <gpu_generated.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace meminfo {
namespace gpu {

GpuClient::GpuClient(const std::string& ip, int port) {
    uv_loop_init(&loop_);
    
    peer_ = std::make_unique<RemotePeer>();
    peer_->ip = ip;
    peer_->port = port;
    
    uv_async_init(&loop_, &stop_async_, [](uv_async_t* handle) {
        auto* self = static_cast<GpuClient*>(handle->data);
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
        auto* self = static_cast<GpuClient*>(handle->data);
        std::queue<OutboundMessage> batch;
        {
            std::lock_guard<std::mutex> lock(self->queue_mutex_);
            std::swap(batch, self->outbound_queue_);
        }
        
        while (!batch.empty()) {
            auto msg = std::move(batch.front());
            batch.pop();
            
            if (self->peer_ && self->peer_->connected) {
                struct WriteReq {
                    uv_write_t req;
                    std::vector<uint8_t> buf_data;
                };
                
                auto* wr = new WriteReq;
                wr->buf_data = std::move(msg.payload);
                uv_buf_t buf = uv_buf_init(reinterpret_cast<char*>(wr->buf_data.data()),
                                           static_cast<unsigned int>(wr->buf_data.size()));
                
                uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(self->peer_->socket), &buf, 1, [](uv_write_t* req, int) {
                    delete static_cast<WriteReq*>(req->data);
                });
                wr->req.data = wr;
            }
        }
    });
    wakeup_async_.data = this;
    
    peer_->socket = new uv_tcp_t;
    uv_tcp_init(&loop_, peer_->socket);
    peer_->socket->data = this; // Store client pointer in socket data
    
    struct sockaddr_in dest;
    uv_ip4_addr(peer_->ip.c_str(), peer_->port, &dest);
    
    uv_connect_t* conn = new uv_connect_t;
    conn->data = this;
    uv_tcp_connect(conn, peer_->socket, reinterpret_cast<const struct sockaddr*>(&dest), [](uv_connect_t* req, int status) {
        auto* self = static_cast<GpuClient*>(req->data);
        if (status == 0) {
            self->peer_->connected = true;
            uv_read_start(reinterpret_cast<uv_stream_t*>(self->peer_->socket), 
                [](uv_handle_t*, size_t suggested, uv_buf_t* b) {
                    b->base = new char[suggested];
                    b->len = suggested;
                },
                GpuClient::on_peer_read);
        } else {
            spdlog::error("GpuClient failed to connect to gpud: {}", uv_strerror(status));
        }
        delete req;
    });
    
    network_thread_ = std::thread(&GpuClient::network_thread_main, this);
}

GpuClient::~GpuClient() {
    running_ = false;
    uv_async_send(&stop_async_);
    if (network_thread_.joinable()) {
        network_thread_.join();
    }
    uv_loop_close(&loop_);
}

void GpuClient::network_thread_main() {
    uv_run(&loop_, UV_RUN_DEFAULT);
}

void GpuClient::on_peer_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* self = static_cast<GpuClient*>(stream->data);
    
    if (nread > 0) {
        self->peer_->read_buffer.insert(self->peer_->read_buffer.end(), buf->base, buf->base + nread);
        
        while (self->peer_->read_buffer.size() >= 4) {
            uint32_t msg_size = flatbuffers::GetPrefixedSize(self->peer_->read_buffer.data());
            if (self->peer_->read_buffer.size() >= msg_size + 4) {
                const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::gpu::GpuResponse>(self->peer_->read_buffer.data());
                
                std::shared_ptr<RequestContext> req_ctx;
                {
                    std::lock_guard<std::mutex> lock(self->requests_mutex_);
                    auto it = self->pending_requests_.find(resp->request_id());
                    if (it != self->pending_requests_.end()) {
                        req_ctx = it->second;
                        self->pending_requests_.erase(it);
                    }
                }
                
                if (req_ctx) {
                    std::vector<uint8_t> result;
                    result.assign(self->peer_->read_buffer.data(), self->peer_->read_buffer.data() + msg_size + 4);
                    req_ctx->promise.set_value(std::move(result));
                }
                
                self->peer_->read_buffer.erase(self->peer_->read_buffer.begin(), self->peer_->read_buffer.begin() + msg_size + 4);
            } else {
                break;
            }
        }
    }
    if (buf->base) delete[] buf->base;
}

std::vector<uint8_t> GpuClient::sync_remote_call(const uint8_t* payload, size_t size, uint64_t request_id) {
    auto req_ctx = std::make_shared<RequestContext>();
    req_ctx->request_id = request_id;
    auto future = req_ctx->promise.get_future();
    
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        pending_requests_[request_id] = req_ctx;
    }
    
    OutboundMessage msg;
    msg.payload.assign(payload, payload + size);
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        outbound_queue_.push(std::move(msg));
    }
    uv_async_send(&wakeup_async_);
    
    // Wait for response, could add timeout in real app
    return future.get();
}

int GpuClient::cudaMalloc(uint64_t* devPtr, size_t size) {
    uint64_t req_id = next_request_id_++;
    flatbuffers::FlatBufferBuilder builder;
    meminfo::gpu::GpuRequestBuilder grb(builder);
    grb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
    grb.add_request_id(req_id);
    grb.add_op(meminfo::gpu::GpuOpCode_CUDA_MALLOC);
    grb.add_size(size);
    builder.FinishSizePrefixed(grb.Finish());
    
    auto resp_data = sync_remote_call(builder.GetBufferPointer(), builder.GetSize(), req_id);
    const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::gpu::GpuResponse>(resp_data.data());
    
    if (resp->error_code() == 0) {
        *devPtr = resp->device_ptr();
    }
    return resp->error_code();
}

int GpuClient::cudaFree(uint64_t devPtr) {
    uint64_t req_id = next_request_id_++;
    flatbuffers::FlatBufferBuilder builder;
    meminfo::gpu::GpuRequestBuilder grb(builder);
    grb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
    grb.add_request_id(req_id);
    grb.add_op(meminfo::gpu::GpuOpCode_CUDA_FREE);
    grb.add_device_ptr(devPtr);
    builder.FinishSizePrefixed(grb.Finish());
    
    auto resp_data = sync_remote_call(builder.GetBufferPointer(), builder.GetSize(), req_id);
    const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::gpu::GpuResponse>(resp_data.data());
    return resp->error_code();
}

int GpuClient::cudaMemcpyHtoD(uint64_t dst, const void* src, size_t size) {
    uint64_t req_id = next_request_id_++;
    flatbuffers::FlatBufferBuilder builder;
    auto data_vec = builder.CreateVector(reinterpret_cast<const uint8_t*>(src), size);
    
    meminfo::gpu::GpuRequestBuilder grb(builder);
    grb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
    grb.add_request_id(req_id);
    grb.add_op(meminfo::gpu::GpuOpCode_CUDA_MEMCPY);
    grb.add_dst_ptr(dst);
    grb.add_memcpy_kind(meminfo::gpu::MemcpyKind_HOST_TO_DEVICE);
    grb.add_size(size);
    grb.add_data(data_vec);
    builder.FinishSizePrefixed(grb.Finish());
    
    auto resp_data = sync_remote_call(builder.GetBufferPointer(), builder.GetSize(), req_id);
    const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::gpu::GpuResponse>(resp_data.data());
    return resp->error_code();
}

int GpuClient::cudaMemcpyDtoH(void* dst, uint64_t src, size_t size) {
    uint64_t req_id = next_request_id_++;
    flatbuffers::FlatBufferBuilder builder;
    
    meminfo::gpu::GpuRequestBuilder grb(builder);
    grb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
    grb.add_request_id(req_id);
    grb.add_op(meminfo::gpu::GpuOpCode_CUDA_MEMCPY);
    grb.add_src_ptr(src);
    grb.add_memcpy_kind(meminfo::gpu::MemcpyKind_DEVICE_TO_HOST);
    grb.add_size(size);
    builder.FinishSizePrefixed(grb.Finish());
    
    auto resp_data = sync_remote_call(builder.GetBufferPointer(), builder.GetSize(), req_id);
    const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::gpu::GpuResponse>(resp_data.data());
    
    if (resp->error_code() == 0 && resp->data()) {
        std::memcpy(dst, resp->data()->data(), std::min(size, static_cast<size_t>(resp->data()->size())));
    }
    return resp->error_code();
}

int GpuClient::cudaGetDeviceProperties(std::string& name, size_t& totalGlobalMem) {
    uint64_t req_id = next_request_id_++;
    flatbuffers::FlatBufferBuilder builder;
    
    meminfo::gpu::GpuRequestBuilder grb(builder);
    grb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
    grb.add_request_id(req_id);
    grb.add_op(meminfo::gpu::GpuOpCode_CUDA_GET_DEVICE_PROPERTIES);
    builder.FinishSizePrefixed(grb.Finish());
    
    auto resp_data = sync_remote_call(builder.GetBufferPointer(), builder.GetSize(), req_id);
    const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::gpu::GpuResponse>(resp_data.data());
    
    if (resp->error_code() == 0) {
        if (resp->message()) name = resp->message()->str();
        totalGlobalMem = resp->device_ptr();
    }
    return resp->error_code();
}

} // namespace gpu
} // namespace meminfo
