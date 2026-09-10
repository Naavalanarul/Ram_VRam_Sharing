#include <meminfo/memory/memory_protocol.h>
#include <meminfo/memory/page_tracker.h>
#include <meminfo/common/crc32c.h>
#include <meminfo/common/types.h>
#include <meminfo/common/protocol_version.h>
#include <memory_generated.h>
#include <spdlog/spdlog.h>
#include <string>
#include <stdexcept>

namespace meminfo {
namespace memory {

static std::vector<uint8_t> build_response(flatbuffers::FlatBufferBuilder& builder, const meminfo::memory::MemoryRequest* req, 
                                            meminfo::memory::StatusCode status, const std::string& message,
                                            uint64_t handle = 0, const uint8_t* data = nullptr, size_t data_size = 0, uint32_t checksum = 0) {
    auto fb_msg = builder.CreateString(message);
    auto fb_data = data_size > 0 ? builder.CreateVector(data, data_size) : 0;
    
    meminfo::memory::MemoryResponseBuilder mrb(builder);
    mrb.add_request_id(req->request_id());
    mrb.add_status(status);
    mrb.add_message(fb_msg);
    if (handle != 0) mrb.add_handle(handle);
    if (data_size > 0) {
        mrb.add_data(fb_data);
        mrb.add_checksum(checksum);
    }
    builder.FinishSizePrefixed(mrb.Finish());
    
    const uint8_t* buf = builder.GetBufferPointer();
    size_t out_size = builder.GetSize();
    return std::vector<uint8_t>(buf, buf + out_size);
}

std::vector<uint8_t> MemoryProtocol::process_request(PageTracker* page_tracker, uint64_t session_id, const uint8_t* data, size_t size) {
    flatbuffers::Verifier verifier(data, size);
    if (!meminfo::memory::VerifySizePrefixedMemoryRequestBuffer(verifier)) {
        spdlog::warn("Invalid MemoryRequest buffer received");
        return {}; // Empty vector means invalid, caller should close connection
    }

    const auto* req = flatbuffers::GetSizePrefixedRoot<meminfo::memory::MemoryRequest>(data);
    if (!check_protocol_version(req->protocol_version())) {
        flatbuffers::FlatBufferBuilder builder;
        return build_response(builder, req, meminfo::memory::StatusCode_VERSION_MISMATCH, "Protocol version mismatch");
    }

    flatbuffers::FlatBufferBuilder builder;

    try {
        switch (req->op()) {
            case meminfo::memory::OpCode_ALLOC: {
                handle_t handle = page_tracker->allocate(req->size());
                page_tracker->assign_owner(handle, session_id);
                return build_response(builder, req, meminfo::memory::StatusCode_OK, "OK", handle);
            }
            
            case meminfo::memory::OpCode_FREE: {
                page_tracker->free(req->handle(), session_id);
                return build_response(builder, req, meminfo::memory::StatusCode_OK, "OK");
            }
            
            case meminfo::memory::OpCode_WRITE: {
                if (!req->data()) {
                    return build_response(builder, req, meminfo::memory::StatusCode_ERROR_GENERIC, "Missing data payload");
                }
                uint32_t computed_crc = crc32c(req->data()->data(), req->data()->size());
                if (computed_crc != req->checksum()) {
                    return build_response(builder, req, meminfo::memory::StatusCode_CHECKSUM_MISMATCH, "CRC32C mismatch");
                }
                page_tracker->write(req->handle(), req->offset(), req->data()->data(), req->data()->size(), session_id);
                return build_response(builder, req, meminfo::memory::StatusCode_OK, "OK");
            }
            
            case meminfo::memory::OpCode_READ: {
                // req->size() is attacker-controlled; reserving it before the
                // handle is validated would let one request ask for gigabytes.
                if (req->size() > MAX_MESSAGE_SIZE) {
                    return build_response(builder, req, meminfo::memory::StatusCode_ERROR_GENERIC,
                                          "Requested read size exceeds maximum message size");
                }
                std::vector<uint8_t> read_data(req->size());
                page_tracker->read(req->handle(), req->offset(), read_data.data(), req->size(), session_id);
                uint32_t checksum = crc32c(read_data.data(), read_data.size());
                return build_response(builder, req, meminfo::memory::StatusCode_OK, "OK", 0, read_data.data(), read_data.size(), checksum);
            }
            
            case meminfo::memory::OpCode_PING: {
                return build_response(builder, req, meminfo::memory::StatusCode_OK, "PONG");
            }
            
            default: {
                return build_response(builder, req, meminfo::memory::StatusCode_ERROR_GENERIC, "Unsupported operation");
            }
        }
    } catch (const PermissionDeniedError& e) {
        return build_response(builder, req, meminfo::memory::StatusCode_PERMISSION_DENIED, e.what());
    } catch (const std::invalid_argument& e) {
        return build_response(builder, req, meminfo::memory::StatusCode_INVALID_HANDLE, e.what());
    } catch (const std::bad_alloc& e) {
        return build_response(builder, req, meminfo::memory::StatusCode_OUT_OF_MEMORY, "Not enough contiguous free pages");
    } catch (const std::exception& e) {
        return build_response(builder, req, meminfo::memory::StatusCode_ERROR_GENERIC, e.what());
    }
}

} // namespace memory
} // namespace meminfo