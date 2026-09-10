#include <meminfo/gpu/gpu_session.h>
#include <meminfo/gpu/gpu_protocol.h>

#include <meminfo/gpu/cuda_executor.h>
#include <meminfo/common/protocol_version.h>
#include <gpu_generated.h>
#include <spdlog/spdlog.h>
#include <meminfo/common/types.h>
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
            spdlog::error("GpuSession {} read error: {}", self->session_id_, uv_strerror(static_cast<int>(nread)));
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
        if (size > MAX_MESSAGE_SIZE) {
            spdlog::error("GpuSession {} received oversized message", session_id_);
            if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&socket_))) {
                uv_close(reinterpret_cast<uv_handle_t*>(&socket_), on_close);
            }
            return;
        }
        if (read_buffer_.size() - 4 >= size) {
            handle_request(read_buffer_.data(), size + 4);
            read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + size + 4);
        } else {
            break;
        }
    }
}

void GpuSession::handle_request(const uint8_t* data, size_t size) {
    auto response = GpuProtocol::process_request(executor_, data, size);
    if (response.empty()) {
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&socket_))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&socket_), on_close);
        }
        return;
    }

    if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&socket_))) return;
    
    WriteReqCtx* ctx = new WriteReqCtx;
    ctx->buf_base = new char[response.size()];
    std::memcpy(ctx->buf_base, response.data(), response.size());
    ctx->req.data = ctx;
    
    uv_buf_t buf = uv_buf_init(ctx->buf_base, static_cast<unsigned int>(response.size()));
    uv_write(&ctx->req, reinterpret_cast<uv_stream_t*>(&socket_), &buf, 1, on_write_done);
}

void GpuSession::on_write_done(uv_write_t* req, int /*status*/) {
    WriteReqCtx* ctx = static_cast<WriteReqCtx*>(req->data);
    delete[] ctx->buf_base;
    delete ctx;
}

} // namespace gpu
} // namespace meminfo
