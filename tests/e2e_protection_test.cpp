#include "e2e_protection.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>

namespace {

using lab::e2e::Frame;

Frame make_frame()
{
    Frame frame;
    frame.counter = 7;
    frame.data_id = 0x0000ABCD;
    frame.data = "plant-data";
    return frame;
}

}  // namespace

TEST(E2eProtectionTest, Crc32MatchesKnownVector)
{
    // CRC-32/ISO-HDLC 对 "123456789" 的标准结果
    const std::string input = "123456789";
    EXPECT_EQ(lab::e2e::crc32(input), 0xCBF43926u);
}

TEST(E2eProtectionTest, ProtectWritesCounterAndDataId)
{
    const Frame frame = make_frame();
    const std::string bytes = lab::e2e::protect(frame);

    ASSERT_EQ(bytes.size(), lab::e2e::kHeaderSize + frame.data.size());
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[0]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[1]), 0x07);
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[2]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[3]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[4]), 0xAB);
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[5]), 0xCD);
}

TEST(E2eProtectionTest, RoundTripRecoversAllFields)
{
    const Frame frame = make_frame();
    const std::string bytes = lab::e2e::protect(frame);

    Frame decoded;
    ASSERT_TRUE(lab::e2e::unprotect(bytes, decoded));
    EXPECT_EQ(decoded.counter, frame.counter);
    EXPECT_EQ(decoded.data_id, frame.data_id);
    EXPECT_EQ(decoded.data, frame.data);
}

TEST(E2eProtectionTest, RejectsCorruptedData)
{
    std::string bytes = lab::e2e::protect(make_frame());
    bytes.back() = static_cast<char>(bytes.back() ^ 0x01);  // 翻转数据区最后一位

    Frame decoded;
    EXPECT_FALSE(lab::e2e::unprotect(bytes, decoded));
}

TEST(E2eProtectionTest, RejectsCorruptedCounter)
{
    std::string bytes = lab::e2e::protect(make_frame());
    bytes[1] = static_cast<char>(bytes[1] ^ 0x01);  // 翻转 Counter 低位

    Frame decoded;
    EXPECT_FALSE(lab::e2e::unprotect(bytes, decoded));
}

TEST(E2eProtectionTest, RejectsShortInput)
{
    const std::string bytes(lab::e2e::kHeaderSize - 1, '\0');
    Frame decoded;
    EXPECT_FALSE(lab::e2e::unprotect(bytes, decoded));
}

TEST(E2eProtectionTest, HandlesEmptyData)
{
    Frame frame;
    frame.counter = 1;
    frame.data_id = 0x12345678;
    // data 为空

    const std::string bytes = lab::e2e::protect(frame);
    ASSERT_EQ(bytes.size(), lab::e2e::kHeaderSize);

    Frame decoded;
    ASSERT_TRUE(lab::e2e::unprotect(bytes, decoded));
    EXPECT_EQ(decoded.counter, 1u);
    EXPECT_EQ(decoded.data_id, 0x12345678u);
    EXPECT_TRUE(decoded.data.empty());
}