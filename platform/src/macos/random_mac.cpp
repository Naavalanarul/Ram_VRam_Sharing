#include <meminfo/platform/random.h>
#include <stdlib.h>

namespace meminfo {
namespace platform {

void generate_random_bytes(uint8_t* buffer, size_t size) {
    arc4random_buf(buffer, size);
}

} // namespace platform
} // namespace meminfo
