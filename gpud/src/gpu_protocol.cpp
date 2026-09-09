#include <meminfo/gpu/gpu_protocol.h>
#include <meminfo/common/protocol_version.h>
#include <gpu_generated.h>
#include <spdlog/spdlog.h>
#include <string>

namespace meminfo {
namespace gpu {

static std::vector<uint8_t> build_response(uint64_t request_id, int error_code, uint64_t device_ptr, const uint8_t* data, size_t data_size, const std::string& message) {
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
    
    const uint8_t* buf = builder.GetBufferPointer();
    size_t out_size = builder.GetSize();
    return std::vector<uint8_t>(buf, buf + out_size);
}

std::vector<uint8_t> GpuProtocol::process_request(ICudaExecutor* executor, const uint8_t* data, size_t size) {
    flatbuffers::Verifier verifier(data, size);
    if (!meminfo::gpu::VerifySizePrefixedGpuRequestBuffer(verifier)) {
        spdlog::warn("Invalid GpuRequest buffer received");
        return {}; // Empty vector means invalid, caller should close connection
    }

    const auto* req = flatbuffers::GetSizePrefixedRoot<meminfo::gpu::GpuRequest>(data);
    if (!check_protocol_version(req->protocol_version())) {
        return build_response(req->request_id(), -1, 0, nullptr, 0, "Protocol version mismatch");
    }

    int err = 0;
    uint64_t result_ptr = 0;
    std::vector<uint8_t> result_data;
    std::string result_msg;

    switch (req->op()) {
        case meminfo::gpu::GpuOpCode_CUDA_MALLOC: {
            err = executor->allocate(&result_ptr, req->size());
            break;
        }
        case meminfo::gpu::GpuOpCode_CUDA_FREE: {
            err = executor->free(req->device_ptr());
            break;
        }
        case meminfo::gpu::GpuOpCode_CUDA_MEMCPY: {
            if (req->memcpy_kind() == meminfo::gpu::MemcpyKind_HOST_TO_DEVICE) {
                if (req->data()) {
                    err = executor->memcpyHtoD(req->dst_ptr(), req->data()->data(), req->size());
                } else {
                    err = 11; // cudaErrorInvalidValue
                }
            } else if (req->memcpy_kind() == meminfo::gpu::MemcpyKind_DEVICE_TO_HOST) {
                result_data.resize(req->size());
                err = executor->memcpyDtoH(result_data.data(), req->src_ptr(), req->size());
            } else {
                err = 11; // DeviceToDevice not implemented
            }
            break;
        }
        case meminfo::gpu::GpuOpCode_CUDA_GET_DEVICE_PROPERTIES: {
            std::string name;
            size_t total_mem = 0;
            err = executor->getDeviceProperties(name, total_mem);
            if (err == 0) {
                result_msg = name;
                result_ptr = total_mem;
            }
            break;
        }
        default:
            err = 1;
            result_msg = "Unsupported operation";
            break;
    }

    return build_response(req->request_id(), err, result_ptr, result_data.empty() ? nullptr : result_data.data(), result_data.size(), result_msg);
}

} // namespace gpu
} // namespace meminfo
