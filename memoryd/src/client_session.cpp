#include <meminfo/common/types.h>

#include <meminfo/memory/client_session.h>
#include <meminfo/memory/page_tracker.h>
#include <meminfo/memory/memory_protocol.h>
#include <memory_generated.h>
#include <spdlog/spdlog.h>

namespace meminfo {
namespace memory {

ClientSession::ClientSession(uv_loop_t* loop, uv_stream_t* server, PageTracker* page_tracker, uint64_t session_id)
    : page_tracker_(page_tracker), session_id_(session_id) {
    
    uv_tcp_init(loop, &socket_);
    socket_.data = this;
    
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&socket_)) == 0) {
        uv_read_start(reinterpret_cast<uv_stream_t*>(&socket_), on_alloc, on_read);
    } else {
        uv_close(reinterpret_cast<uv_handle_t*>(&socket_), on_close);
    }
}

ClientSession::~ClientSession() {
    page_tracker_->free_all_for_owner(session_id_);
}

void ClientSession::on_alloc(uv_handle_t* /*handle*/, size_t suggested_size, uv_buf_t* buf) {
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

void ClientSession::on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* self = static_cast<ClientSession*>(stream->data);
    
    if (nread > 0) {
        self->read_buffer_.insert(self->read_buffer_.end(), buf->base, buf->base + nread);
        self->process_buffer();
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            spdlog::error("ClientSession read error: {}", uv_strerror(nread));
        }
        uv_close(reinterpret_cast<uv_handle_t*>(stream), on_close);
    }
    
    if (buf->base) {
        delete[] buf->base;
    }
}

void ClientSession::process_buffer() {
    while (read_buffer_.size() >= 4) {
        uint32_t size = flatbuffers::GetPrefixedSize(read_buffer_.data());
        if (size > MAX_MESSAGE_SIZE) {
            spdlog::error("ClientSession received oversized message");
            uv_close(reinterpret_cast<uv_handle_t*>(&socket_), on_close);
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

void ClientSession::handle_request(const uint8_t* data, size_t size) {
    // Delegate to MemoryProtocol for request processing
    auto response = MemoryProtocol::process_request(page_tracker_, session_id_, data, size);
    
    // Empty response means invalid protocol - close connection
    if (response.empty()) {
        uv_close(reinterpret_cast<uv_handle_t*>(&socket_), on_close);
        return;
    }
    
    send_response(response.data(), response.size());
}

struct WriteReqCtx {
    uv_write_t req;
    char* buf_base;
};

void ClientSession::send_response(const uint8_t* data, size_t size) {
    if (uv_is_closing(reinterpret_cast<uv_handle_t*>(&socket_))) return;
    
    WriteReqCtx* ctx = new WriteReqCtx;
    ctx->buf_base = new char[size];
    std::memcpy(ctx->buf_base, data, size);
    ctx->req.data = ctx;
    
    uv_buf_t buf = uv_buf_init(ctx->buf_base, size);
    uv_write(&ctx->req, reinterpret_cast<uv_stream_t*>(&socket_), &buf, 1, on_write_done);
}

void ClientSession::on_write_done(uv_write_t* req, int /*status*/) {
    WriteReqCtx* ctx = static_cast<WriteReqCtx*>(req->data);
    delete[] ctx->buf_base;
    delete ctx;
}

void ClientSession::on_close(uv_handle_t* handle) {
    auto* self = static_cast<ClientSession*>(handle->data);
    delete self; // ClientSession owns itself once accepted
}

} // namespace memory
} // namespace meminfo
