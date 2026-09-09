#include "meminfo/common/uuid.h"
#include <meminfo/platform/random.h>
#include <stdexcept>
#include <array>

namespace meminfo {

node_id_t generate_uuid() {
    node_id_t uuid;
    platform::generate_random_bytes(uuid.data(), uuid.size());

    // Set version 4
    uuid[6] = (uuid[6] & 0x0f) | 0x40;
    // Set variant 1
    uuid[8] = (uuid[8] & 0x3f) | 0x80;

    return uuid;
}

} // namespace meminfo
