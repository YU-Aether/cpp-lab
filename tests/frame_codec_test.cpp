#include "frame_codec.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

// 独立构造头部，避免用被测代码生成测试输入。
std::string make_header(std::size_t length)
{
    const auto value = static_cast<std::uint32_t>(length);

    std::string header;
    header.push_back(static_cast<char>((value >> 24) & 0xFF));
    header.push_back(static_cast<char>((value >> 16) & 0xFF));
    header.push_back(static_cast<char>((value >> 8) & 0xFF));
    header.push_back(static_cast<char>(value & 0xFF));
    return header;
}

}  // namespace

TEST(FrameCodecTest, EncodeWritesBigEndianLength)
{
    const std::string frame = lab::encode_frame("abc");

    ASSERT_EQ(frame.size(), 7u);
    EXPECT_EQ(frame[0], '\0');
    EXPECT_EQ(frame[1], '\0');
    EXPECT_EQ(frame[2], '\0');
    EXPECT_EQ(frame[3], '\3');
    EXPECT_EQ(frame.substr(4), "abc");
}

TEST(FrameCodecTest, DecodesSingleFrame)
{
    lab::FrameDecoder decoder;
    decoder.append(lab::encode_frame("hello"));

    std::string frame;
    ASSERT_TRUE(decoder.next_frame(frame));
    EXPECT_EQ(frame, "hello");

    EXPECT_FALSE(decoder.next_frame(frame));
    EXPECT_EQ(decoder.buffered_size(), 0u);
}

TEST(FrameCodecTest, DecodesTwoFramesFromOneAppend)
{
    lab::FrameDecoder decoder;
    decoder.append(lab::encode_frame("first") + lab::encode_frame("second"));

    std::string frame;
    ASSERT_TRUE(decoder.next_frame(frame));
    EXPECT_EQ(frame, "first");

    ASSERT_TRUE(decoder.next_frame(frame));
    EXPECT_EQ(frame, "second");

    EXPECT_FALSE(decoder.next_frame(frame));
}

TEST(FrameCodecTest, WaitsForCompleteHeader)
{
    const std::string encoded = lab::encode_frame("payload");
    lab::FrameDecoder decoder;

    // 头部只有 2 字节，不足以解析长度。
    decoder.append(encoded.substr(0, 2));

    std::string frame;
    EXPECT_FALSE(decoder.next_frame(frame));
    EXPECT_EQ(decoder.buffered_size(), 2u);

    decoder.append(encoded.substr(2));
    ASSERT_TRUE(decoder.next_frame(frame));
    EXPECT_EQ(frame, "payload");
    EXPECT_EQ(decoder.buffered_size(), 0u);
}

TEST(FrameCodecTest, WaitsForCompletePayload)
{
    const std::string encoded = lab::encode_frame("abcdef");
    lab::FrameDecoder decoder;

    // 4 字节头部 + 2 字节载荷，载荷尚未收全。
    decoder.append(encoded.substr(0, 6));

    std::string frame;
    EXPECT_FALSE(decoder.next_frame(frame));
    EXPECT_EQ(decoder.buffered_size(), 6u);

    decoder.append(encoded.substr(6));
    ASSERT_TRUE(decoder.next_frame(frame));
    EXPECT_EQ(frame, "abcdef");
}

TEST(FrameCodecTest, HandlesByteByByteAppend)
{
    const std::string encoded = lab::encode_frame("split");
    lab::FrameDecoder decoder;

    std::string frame;
    bool got_frame = false;

    for (std::size_t index = 0; index < encoded.size(); ++index) {
        decoder.append(encoded.data() + index, 1);

        if (decoder.next_frame(frame)) {
            got_frame = true;
            break;
        }
    }

    ASSERT_TRUE(got_frame);
    EXPECT_EQ(frame, "split");
    EXPECT_EQ(decoder.buffered_size(), 0u);
}

TEST(FrameCodecTest, DecodesEmptyPayload)
{
    lab::FrameDecoder decoder;
    decoder.append(lab::encode_frame(""));

    std::string frame = "stale";
    ASSERT_TRUE(decoder.next_frame(frame));
    EXPECT_TRUE(frame.empty());
    EXPECT_EQ(decoder.buffered_size(), 0u);
}

TEST(FrameCodecTest, PreservesBinaryPayload)
{
    std::string payload;
    payload.push_back('\0');
    payload.push_back('A');
    payload.push_back(static_cast<char>(0xFF));
    payload.push_back('\0');

    lab::FrameDecoder decoder;
    decoder.append(lab::encode_frame(payload));

    std::string frame;
    ASSERT_TRUE(decoder.next_frame(frame));
    EXPECT_EQ(frame, payload);
    EXPECT_EQ(frame.size(), payload.size());
}

TEST(FrameCodecTest, RejectsOversizedLength)
{
    // 上限设为 4 字节，构造声明 5 字节的帧头。
    lab::FrameDecoder decoder(4);
    decoder.append(make_header(5) + "12345");

    std::string frame;
    EXPECT_THROW(decoder.next_frame(frame), std::runtime_error);

    // 出错后缓冲被清空，解码器可以被丢弃。
    EXPECT_EQ(decoder.buffered_size(), 0u);
    EXPECT_FALSE(decoder.next_frame(frame));
}

TEST(FrameCodecTest, AcceptsLengthEqualToLimit)
{
    lab::FrameDecoder decoder(4);
    decoder.append(lab::encode_frame("1234"));

    std::string frame;
    ASSERT_TRUE(decoder.next_frame(frame));
    EXPECT_EQ(frame, "1234");
}