#include <meminfo/gpu/gpu_session.h>
#include <meminfo/gpu/cuda_executor.h>
#include <meminfo/common/protocol_version.h>
#include <gpu_generated.h>
#include <spdlog/spdlog.h>
#include <cstring>

namespace meminfo {
namespace gpu {

GpuSession::GpuSession(uv_loop_t* loop, uv_stream_t* server, ICudaExecutor* executor, uint64_t session_id)
    : executor_(executor), session_id_(session_id) {
    
    uv_tcp_init(loop, &socket_);
    socket_.data = this;
    
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&socket_)) == 0) {
        uv_read_start(reinterpret_cast<uv_stream_t*>(&socket_), on_alloc, on_read);
    } else {
        uv_close(reinterpret_cast<uv_handle_t*>(&socket_), on_close);
    }
}

GpuSession::~GpuSession() {
    // If the client leaked memory, we can't really track it here automatically unless 
    // we build a tracker like PageTracker. For now, we assume explicit frees.
}

void GpuSession::on_alloc(uv_handle_t* /*handle*/, size_t suggested_size, uv_buf_t* buf) {
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

void GpuSession::on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* self = static_cast<GpuSession*>(stream->data);
    
    if (nread > 0) {
        self->read_buffer_.insert(self->read_buffer_.end(), buf->base, buf->base + nread);
        self->process_buffer();
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            spdlog::error("GpuSession {} read error: {}", self->session_id_, uv_strerror(nread));
        }
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(stream))) {
            uv_close(reinterpret_cast<uv_handle_t*>(stream), on_close);
        }
    }
    
    if (buf->base) delete[] buf->base;
}

void GpuSession::on_close(uv_handle_t* handle) {
    auto* self = static_cast<GpuSession*>(handle->data);
    delete self;
}

void GpuSession::process_buffer() {
    while (read_buffer_.size() >= 4) {
        uint32_t size = flatbuffers::GetPrefixedSize(read_buffer_.data());
        if (read_buffer_.size() >= size + 4) {
            handle_request(read_buffer_.data(), size + 4);
            read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + size + 4);
        } else {
            break;
        }
    }
}

void GpuSession::handle_request(const uint8_t* data, size_t size) {
    flatbuffers::Verifier verifier(data, size);
    if (!meminfo::gpu::VerifySizePrefixedGpuRequestBuffer(verifier)) {
        spdlog::warn("Invalid GpuRequest buffer received");
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&socket_))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&socket_), on_close);
        }
        return;
    }

    const auto* req = flatbuffers::GetSizePrefixedRoot<meminfo::gpu::GpuRequest>(data);
    if (!check_protocol_version(req->protocol_version())) {
        send_response(req->request_id(), -1, 0, nullptr, 0, "Protocol version mismatch");
        return;
    }

    int err = 0;
    uint64_t result_ptr = 0;
    std::vector<uint8_t> result_data;
    std::string result_msg;

    switch (req->op()) {
        case meminfo::gpu::GpuOpCode_CUDA_MALLOC: {
            err = executor_->allocate(&result_ptr, req->size());
            break;
        }
        case meminfo::gpu::GpuOpCode_CUDA_FREE: {
            err = executor_->free(req->device_ptr());
            break;
        }
        case meminfo::gpu::GpuOpCode_CUDA_MEMCPY: {
            if (req->memcpy_kind() == meminfo::gpu::MemcpyKind_HOST_TO_DEVICE) {
                if (req->data()) {
                    err = executor_->memcpyHtoD(req->dst_ptr(), req->data()->data(), req->size());
                } else {
                    err = 11; // cudaErrorInvalidValue
                }
            } else if (req->memcpy_kind() == meminfo::gpu::MemcpyKind_DEVICE_TO_HOST) {
                result_data.resize(req->size());
                err = executor_->memcpyDtoH(result_data.data(), req->src_ptr(), req->size());
            } else {
                err = 11; // DeviceToDevice not implemented in this demo
            }
            break;
        }
        case meminfo::gpu::GpuOpCode_CUDA_GET_DEVICE_PROPERTIES: {
            std::string name;
            size_t total_mem = 0;
            err = executor_->getDeviceProperties(name, total_mem);
            if (err == 0) {
                result_msg = name;
                result_ptr = total_mem; // Hacky way to return total mem without extending flatbuffers
            }
            break;
        }
        default:
            err = 1; // Unknown op
            result_msg = "Unsupported operation";
            break;
    }

    send_response(req->request_id(), err, result_ptr, result_data.empty() ? nullptr : result_data.data(), result_data.size(), result_msg);
}

void GpuSession::send_response(uint64_t request_id, int error_code, uint64_t device_ptr, const uint8_t* data, size_t data_size, const std::string& message) {
    if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&socket_))) return;
    
    flatbuffers::FlatBufferBuilder builder;
    auto fb_msg = builder.CreateString(message);
    auto fb_data = data_size > 0 ? builder.CreateVector(data, data_size) : 0;
    
    meminfo::gpu::GpuResponseBuilder grb(builder);
    grb.add_request_id(request_id);
    grb.add_error_code(error_code);
    grb.add_device_ptr(device_ptr);
    if (data_size > 0) grb.add_data(fb_data);
    grb.add_message(fb_msg);
    builder.FinishSizePrefixed(grb.Finish());
    
    size_t out_size = builder.GetSize();
    WriteReqCtx* ctx = new WriteReqCtx;
    ctx->buf_base = new char[out_size];
    std::memcpy(ctx->buf_base, builder.GetBufferPointer(), out_size);
    ctx->req.data = ctx;
    
    uv_buf_t buf = uv_buf_init(ctx->buf_base, out_size);
    uv_write(&ctx->req, reinterpret_cast<uv_stream_t*>(&socket_), &buf, 1, on_write_done);
}

void GpuSession::on_write_done(uv_write_t* req, int /*status*/) {
    WriteReqCtx* ctx = static_cast<WriteReqCtx*>(req->data);
    delete[] ctx->buf_base;
    delete ctx;
}

} // namespace gpu
} // namespace meminfo
