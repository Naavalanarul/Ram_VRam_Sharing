#include <gtest/gtest.h>
#include <meminfo/common/protocol_version.h>

using namespace meminfo;

TEST(ProtocolVersionTest, VersionIsOne) {
    EXPECT_EQ(MEMINFO_PROTOCOL_VERSION, 1);
}

TEST(ProtocolVersionTest, CheckProtocolVersion) {
    EXPECT_TRUE(check_protocol_version(1));
    EXPECT_FALSE(check_protocol_version(0));
    EXPECT_FALSE(check_protocol_version(2));
}

TEST(ProtocolVersionTest, ProtocolVersionString) {
    EXPECT_EQ(protocol_version_string(), "1.0");
}
