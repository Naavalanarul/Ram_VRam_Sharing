#include <meminfo/memory/client_session.h>
#include <meminfo/common/crc32c.h>
#include <meminfo/common/protocol_version.h>
#include <memory_generated.h>
#include <spdlog/spdlog.h>
#include <iostream>

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
        if (read_buffer_.size() >= size + 4) {
            handle_request(read_buffer_.data(), size + 4);
            read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + size + 4);
        } else {
            break;
        }
    }
}

void ClientSession::handle_request(const uint8_t* data, size_t size) {
    flatbuffers::Verifier verifier(data, size);
    if (!meminfo::memory::VerifySizePrefixedMemoryRequestBuffer(verifier)) {
        spdlog::warn("Invalid MemoryRequest buffer received");
        uv_close(reinterpret_cast<uv_handle_t*>(&socket_), on_close);
        return;
    }
    
    const auto* req = meminfo::memory::GetSizePrefixedMemoryRequest(data);
    
    flatbuffers::FlatBufferBuilder builder;
    meminfo::memory::StatusCode status = meminfo::memory::StatusCode_OK;
    std::string message = "OK";
    uint64_t handle = 0;
    std::vector<uint8_t> read_data;
    uint32_t checksum = 0;
    
    if (!check_protocol_version(req->protocol_version())) {
        status = meminfo::memory::StatusCode_VERSION_MISMATCH;
        message = "Protocol version mismatch";
    } else {
        try {
            switch (req->op()) {
                case meminfo::memory::OpCode_ALLOC:
                    handle = page_tracker_->allocate(req->size());
                    page_tracker_->assign_owner(handle, session_id_);
                    break;
                    
                case meminfo::memory::OpCode_FREE:
                    page_tracker_->free(req->handle());
                    break;
                    
                case meminfo::memory::OpCode_WRITE: {
                    if (req->data()) {
                        uint32_t computed_crc = crc32c(req->data()->data(), req->data()->size());
                        if (computed_crc != req->checksum()) {
                            status = meminfo::memory::StatusCode_CHECKSUM_MISMATCH;
                            message = "CRC32C mismatch";
                        } else {
                            page_tracker_->write(req->handle(), req->offset(), req->data()->data(), req->data()->size());
                        }
                    }
                    break;
                }
                    
                case meminfo::memory::OpCode_READ: {
                    read_data.resize(req->size());
                    page_tracker_->read(req->handle(), req->offset(), read_data.data(), req->size());
                    checksum = crc32c(read_data.data(), read_data.size());
                    break;
                }
                    
                case meminfo::memory::OpCode_PING:
                    // Just return OK
                    break;
            }
        } catch (const std::invalid_argument& e) {
            status = meminfo::memory::StatusCode_INVALID_HANDLE;
            message = e.what();
        } catch (const std::bad_alloc& e) {
            status = meminfo::memory::StatusCode_OUT_OF_MEMORY;
            message = "Not enough contiguous free pages";
        } catch (const std::exception& e) {
            status = meminfo::memory::StatusCode_ERROR_GENERIC;
            message = e.what();
        }
    }
    
    auto fb_msg = builder.CreateString(message);
    auto fb_data = read_data.empty() ? 0 : builder.CreateVector(read_data);
    
    meminfo::memory::MemoryResponseBuilder mrb(builder);
    mrb.add_request_id(req->request_id());
    mrb.add_status(status);
    mrb.add_message(fb_msg);
    if (req->op() == meminfo::memory::OpCode_ALLOC) {
        mrb.add_handle(handle);
    }
    if (req->op() == meminfo::memory::OpCode_READ && status == meminfo::memory::StatusCode_OK) {
        mrb.add_data(fb_data);
        mrb.add_checksum(checksum);
    }
    builder.FinishSizePrefixed(mrb.Finish());
    
    send_response(builder.GetBufferPointer(), builder.GetSize());
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
