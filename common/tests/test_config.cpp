#include <gtest/gtest.h>
#include <meminfo/common/config.h>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <filesystem>

using namespace meminfo;

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::ofstream out(test_file);
        out << "[server]\nport = 8080\n";
        out.close();
    }
    
    void TearDown() override {
        std::remove(test_file.c_str());
    }
    
    std::string test_file = (std::filesystem::temp_directory_path() / "meminfo_test_config.toml").string();
};

TEST_F(ConfigTest, DefaultConstructor) {
    Config cfg;
    EXPECT_FALSE(cfg.has("server", "port"));
}

TEST_F(ConfigTest, LoadFromToml) {
    Config cfg(test_file);
    EXPECT_TRUE(cfg.has("server", "port"));
    EXPECT_EQ(cfg.get<int>("server", "port", 9000), 8080);
}

TEST_F(ConfigTest, GetWithDefaults) {
    Config cfg;
    EXPECT_EQ(cfg.get<int>("nonexistent", "port", 9000), 9000);
}

TEST_F(ConfigTest, HasReturnsCorrectResults) {
    Config cfg(test_file);
    EXPECT_TRUE(cfg.has("server", "port"));
    EXPECT_FALSE(cfg.has("server", "missing_key"));
    EXPECT_FALSE(cfg.has("missing_section", "port"));
}

TEST_F(ConfigTest, LoadingInvalidThrows) {
    std::string invalid_file = (std::filesystem::temp_directory_path() / "meminfo_invalid_config.toml").string();
    std::ofstream out(invalid_file);
    out << "[server\nport = ";
    out.close();
    
    EXPECT_THROW({ Config cfg(invalid_file); }, std::runtime_error);
    std::remove(invalid_file.c_str());
}
