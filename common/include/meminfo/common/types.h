#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>

namespace meminfo {

constexpr uint32_t MAX_MESSAGE_SIZE = 64 * 1024 * 1024; // 64MB

using node_id_t = std::array<uint8_t, 16>;
// NB: <windows.h> also declares a global ::handle_t (from the RPC headers),
// so this must be written as meminfo::handle_t anywhere a
// `using namespace meminfo;` is in scope, or MSVC reports it as ambiguous.
using handle_t = uint64_t;
using request_id_t = uint64_t;

inline std::string to_string(const node_id_t& id) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < id.size(); ++i) {
        ss << std::setw(2) << static_cast<int>(id[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            ss << "-";
        }
    }
    return ss.str();
}

} // namespace meminfo

namespace std {
template <>
struct hash<meminfo::node_id_t> {
    size_t operator()(const meminfo::node_id_t& id) const {
        size_t h = 0;
        for (uint8_t b : id) {
            h ^= std::hash<uint8_t>{}(b) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};
} // namespace std
