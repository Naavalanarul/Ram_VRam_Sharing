#include <gtest/gtest.h>
#include <meminfo/common/crc32c.h>
#include <string>

using namespace meminfo;

TEST(Crc32cTest, EmptyDataReturnsZero) {
    EXPECT_EQ(crc32c(nullptr, 0), 0);
}

TEST(Crc32cTest, KnownTestVectors) {
    std::string data = "123456789";
    EXPECT_EQ(crc32c(reinterpret_cast<const uint8_t*>(data.data()), data.size()), 0xE3069283);
}

TEST(Crc32cTest, IncrementalCrc) {
    std::string data1 = "12345";
    std::string data2 = "6789";
    uint32_t crc1 = crc32c(reinterpret_cast<const uint8_t*>(data1.data()), data1.size());
    uint32_t crc2 = crc32c(reinterpret_cast<const uint8_t*>(data2.data()), data2.size(), crc1);
    
    std::string full_data = "123456789";
    uint32_t full_crc = crc32c(reinterpret_cast<const uint8_t*>(full_data.data()), full_data.size());
    EXPECT_EQ(crc2, full_crc);
}

TEST(Crc32cTest, VariousSizes) {
    uint8_t data_1[1] = {0x01};
    EXPECT_NO_THROW(crc32c(data_1, 1));
    
    uint8_t data_7[7] = {0};
    EXPECT_NO_THROW(crc32c(data_7, 7));
    
    uint8_t data_8[8] = {0};
    EXPECT_NO_THROW(crc32c(data_8, 8));
    
    uint8_t data_100[100] = {0};
    EXPECT_NO_THROW(crc32c(data_100, 100));
    
    uint8_t data_4096[4096] = {0};
    EXPECT_NO_THROW(crc32c(data_4096, 4096));
}

TEST(Crc32cTest, SameDataProducesSameChecksum) {
    std::string data = "hello world";
    uint32_t crc1 = crc32c(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    uint32_t crc2 = crc32c(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    EXPECT_EQ(crc1, crc2);
}
