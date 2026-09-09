#pragma once
#include "meminfo/common/types.h"

namespace meminfo {

// Generate a random UUID v4 using /dev/urandom
node_id_t generate_uuid();

} // namespace meminfo
