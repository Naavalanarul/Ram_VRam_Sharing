#include <meminfo/platform/random.h>
#include <sys/random.h>
#include <stdexcept>
#include <system_error>
#include <cerrno>

namespace meminfo {
namespace platform {

void generate_random_bytes(uint8_t* buffer, size_t size) {
    size_t generated = 0;
    while (generated < size) {
        ssize_t result = getrandom(buffer + generated, size - generated, 0);
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "getrandom failed");
        }
        generated += result;
    }
}

} // namespace platform
} // namespace meminfo
