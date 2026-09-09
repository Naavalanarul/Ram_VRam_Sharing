#pragma once
#include <cstddef>
#include <cstdint>

namespace meminfo {

// Compute CRC32C (Castagnoli) checksum.
// Uses hardware SSE 4.2 intrinsics if available, software fallback otherwise.
uint32_t crc32c(const void* data, size_t length, uint32_t initial_crc = 0);

} // namespace meminfo
