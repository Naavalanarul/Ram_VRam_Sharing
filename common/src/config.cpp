#include "meminfo/common/config.h"
#include <stdexcept>

namespace meminfo {

Config::Config(const std::string& filepath) {
    try {
        table_ = toml::parse_file(filepath);
    } catch (const toml::parse_error& err) {
        throw std::runtime_error("TOML Parse Error at " + filepath + ":" + 
                                 std::to_string(err.source().begin.line) + ": " + 
                                 std::string(err.description()));
    }
}

std::string Config::get_string(const std::string& section, const std::string& key, const std::string& default_val) const {
    if (auto node = table_[section][key]) {
        if (auto val = node.value<std::string>()) {
            return *val;
        }
    }
    return default_val;
}

bool Config::has(const std::string& section, const std::string& key) const {
    return !!table_[section][key];
}

} // namespace meminfo
