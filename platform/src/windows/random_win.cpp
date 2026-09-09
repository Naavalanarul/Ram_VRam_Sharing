#include <meminfo/platform/random.h>
#include <windows.h>
#include <bcrypt.h>
#include <stdexcept>

namespace meminfo {
namespace platform {

void generate_random_bytes(uint8_t* buffer, size_t size) {
    NTSTATUS status = BCryptGenRandom(NULL, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) { // 0 is STATUS_SUCCESS
        throw std::runtime_error("BCryptGenRandom failed");
    }
}

} // namespace platform
} // namespace meminfo
