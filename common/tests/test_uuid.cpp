#include <gtest/gtest.h>
#include <meminfo/common/uuid.h>
#include <regex>

using namespace meminfo;

TEST(UuidTest, GenerateReturns16Bytes) {
    auto u = generate_uuid();
    EXPECT_EQ(u.size(), 16);
}

TEST(UuidTest, VersionBitsCorrect) {
    auto u = generate_uuid();
    EXPECT_EQ(u[6] & 0xF0, 0x40);
}

TEST(UuidTest, VariantBitsCorrect) {
    auto u = generate_uuid();
    EXPECT_EQ(u[8] & 0xC0, 0x80);
}

TEST(UuidTest, DistinctUuids) {
    auto u1 = generate_uuid();
    auto u2 = generate_uuid();
    EXPECT_NE(u1, u2);
}

TEST(UuidTest, ToStringFormat) {
    auto u = generate_uuid();
    std::string s = to_string(u);
    std::regex r("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    EXPECT_TRUE(std::regex_match(s, r)) << "UUID string format incorrect: " << s;
}
