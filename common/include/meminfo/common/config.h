#pragma once
#include <string>
#include <cstdint>
#include <toml++/toml.hpp>

namespace meminfo {

// Wraps a TOML config file with typed accessors and defaults.
class Config {
public:
    // Load from file. Throws on parse error.
    explicit Config(const std::string& filepath);
    
    // Default constructor — empty config with defaults only.
    Config() = default;
    
    // Get a value with a default
    template<typename T>
    T get(const std::string& section, const std::string& key, T default_val) const {
        if (auto node = table_[section][key]) {
            if (auto val = node.value<T>()) {
                return *val;
            }
        }
        return default_val;
    }
    
    // Get a string value
    std::string get_string(const std::string& section, const std::string& key, const std::string& default_val) const;
    
    // Check if a key exists
    bool has(const std::string& section, const std::string& key) const;
    
private:
    toml::table table_;
};

} // namespace meminfo
