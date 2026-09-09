#pragma once
#include <cstddef>
#include <cstdint>

namespace meminfo {
namespace platform {

// Fill buffer with cryptographically-secure random bytes.
void generate_random_bytes(uint8_t* buffer, size_t size);

} // namespace platform
} // namespace meminfo
