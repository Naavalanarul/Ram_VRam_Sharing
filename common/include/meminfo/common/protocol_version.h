#pragma once

#include <cstdint>
#include <string>

namespace meminfo {

constexpr uint16_t MEMINFO_PROTOCOL_VERSION = 1;

inline bool check_protocol_version(uint16_t received) {
    return received == MEMINFO_PROTOCOL_VERSION;
}

inline std::string protocol_version_string() {
    return "1.0";
}

} // namespace meminfo
