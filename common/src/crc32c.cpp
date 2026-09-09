#include "meminfo/common/crc32c.h"
#include <array>

#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif

namespace meminfo {

namespace {

constexpr std::array<uint32_t, 256> generate_crc32c_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (uint32_t j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82F63B78;
            } else {
                crc >>= 1;
            }
        }
        table[i] = crc;
    }
    return table;
}

[[maybe_unused]] constexpr auto crc32c_table = generate_crc32c_table();

} // namespace

uint32_t crc32c(const void* data, size_t length, uint32_t initial_crc) {
    uint32_t crc = ~initial_crc;
    const uint8_t* ptr = static_cast<const uint8_t*>(data);

#ifdef __SSE4_2__
    const size_t num_blocks = length / 8;
    const uint64_t* ptr64 = reinterpret_cast<const uint64_t*>(ptr);
    for (size_t i = 0; i < num_blocks; ++i) {
        uint64_t v;
        __builtin_memcpy(&v, ptr64 + i, sizeof(uint64_t));
        crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v));
    }
    
    ptr += num_blocks * 8;
    length -= num_blocks * 8;

    for (size_t i = 0; i < length; ++i) {
        crc = _mm_crc32_u8(crc, ptr[i]);
    }
#else
    for (size_t i = 0; i < length; ++i) {
        crc = crc32c_table[(crc ^ ptr[i]) & 0xFF] ^ (crc >> 8);
    }
#endif

    return ~crc;
}

} // namespace meminfo
