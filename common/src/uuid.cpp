#include "meminfo/common/uuid.h"
#include <fstream>
#include <stdexcept>
#include <array>

namespace meminfo {

node_id_t generate_uuid() {
    std::ifstream urandom("/dev/urandom", std::ios::in | std::ios::binary);
    if (!urandom) {
        throw std::runtime_error("Failed to open /dev/urandom");
    }

    node_id_t uuid;
    urandom.read(reinterpret_cast<char*>(uuid.data()), uuid.size());
    if (!urandom) {
        throw std::runtime_error("Failed to read from /dev/urandom");
    }

    // Set version 4
    uuid[6] = (uuid[6] & 0x0f) | 0x40;
    // Set variant 1
    uuid[8] = (uuid[8] & 0x3f) | 0x80;

    return uuid;
}

} // namespace meminfo
