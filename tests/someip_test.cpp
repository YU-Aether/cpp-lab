#include "someip_codec.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using lab::someip::Message;
using lab::someip::MessageType;
using lab::someip::ReturnCode;

// 构造一条字段全部可知的报文，方便逐字节比对。
Message make_response()
{
    Message msg;
    msg.service_id = 0x1234;
    msg.method_id = 0x5678;
    msg.client_id = 0x9ABC;
    msg.session_id = 0xDEF0;
    msg.interface_version = 0x02;
    msg.message_type = MessageType::Response;
    msg.return_code = ReturnCode::Ok;
    msg.payload = "AB";
    return msg;
}

// 手工拼一个只有头部（外加 extra_tail 个零字节）的原始报文，
// 用来构造各种畸形长度。
std::string make_raw(std::uint32_t length_field, std::size_t extra_tail)
{
    std::string bytes(16, '\0');
    bytes[4] = static_cast<char>((length_field >> 24) & 0xFF);
    bytes[5] = static_cast<char>((length_field >> 16) & 0xFF);
    bytes[6] = static_cast<char>((length_field >> 8) & 0xFF);
    bytes[7] = static_cast<char>(length_field & 0xFF);
    bytes[12] = static_cast<char>(lab::someip::kProtocolVersion);
    bytes.append(extra_tail, '\0');
    return bytes;
}

}  // namespace

TEST(SomeipEncodeTest, ProducesExactHeaderBytes)
{
    const std::string bytes = lab::someip::encode(make_response());
    ASSERT_EQ(bytes.size(), lab::someip::kHeaderSize + 2u);

    const std::vector<std::uint8_t> expected = {
        0x12, 0x34, 0x56, 0x78,  // Message ID：Service 0x1234 + Method 0x5678
        0x00, 0x00, 0x00, 0x0A,  // Length = 8 + 2 = 10
        0x9A, 0xBC, 0xDE, 0xF0,  // Request ID：Client 0x9ABC + Session 0xDEF0
        0x01,                    // Protocol Version
        0x02,                    // Interface Version
        0x80,                    // Message Type = Response
        0x00,                    // Return Code = Ok
        0x41, 0x42,              // "AB"
    };

    ASSERT_EQ(bytes.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(static_cast<std::uint8_t>(bytes[i]), expected[i])
            << "字节下标 " << i;
    }
}

TEST(SomeipEncodeTest, EmptyPayloadLengthIsEight)
{
    Message msg;
    const std::string bytes = lab::someip::encode(msg);

    ASSERT_EQ(bytes.size(), lab::someip::kHeaderSize);
    // Length 从 Request ID 起算，空载荷时正好等于 8
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[4]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[5]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[6]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[7]), 0x08);
}

TEST(SomeipCodecTest, RoundTripPreservesFields)
{
    const Message original = make_response();
    const Message decoded = lab::someip::decode(lab::someip::encode(original));

    EXPECT_EQ(decoded.service_id, original.service_id);
    EXPECT_EQ(decoded.method_id, original.method_id);
    EXPECT_EQ(decoded.client_id, original.client_id);
    EXPECT_EQ(decoded.session_id, original.session_id);
    EXPECT_EQ(decoded.interface_version, original.interface_version);
    EXPECT_EQ(decoded.message_type, original.message_type);
    EXPECT_EQ(decoded.return_code, original.return_code);
    EXPECT_EQ(decoded.payload, original.payload);
}

TEST(SomeipCodecTest, PreservesMessageTypeAndReturnCode)
{
    Message msg = make_response();
    msg.message_type = MessageType::Error;
    msg.return_code = ReturnCode::Timeout;

    const Message decoded = lab::someip::decode(lab::someip::encode(msg));

    EXPECT_EQ(decoded.message_type, MessageType::Error);
    EXPECT_EQ(decoded.return_code, ReturnCode::Timeout);
}

TEST(SomeipDecodeTest, RejectsTruncatedHeader)
{
    const std::string bytes(15, '\0');
    EXPECT_THROW(lab::someip::decode(bytes), std::runtime_error);
}

TEST(SomeipDecodeTest, RejectsLengthBelowEight)
{
    EXPECT_THROW(lab::someip::decode(make_raw(7, 0)), std::runtime_error);
}

TEST(SomeipDecodeTest, RejectsDeclaredLengthMismatch)
{
    // 声明长度 10（即后面还有 10 字节），但实际只给了 1 字节
    EXPECT_THROW(lab::someip::decode(make_raw(10, 1)), std::runtime_error);
}

TEST(SomeipDecodeTest, RejectsWrongProtocolVersion)
{
    std::string bytes = lab::someip::encode(make_response());
    bytes[12] = 0x02;  // 把协议版本改错
    EXPECT_THROW(lab::someip::decode(bytes), std::runtime_error);
}

TEST(SomeipDecoderTest, WaitsForWholeMessage)
{
    const std::string bytes = lab::someip::encode(make_response());
    lab::someip::Decoder decoder;
    Message out;

    // 少一个字节时还不能出报文
    decoder.append(bytes.data(), bytes.size() - 1);
    EXPECT_FALSE(decoder.next_message(out));
    EXPECT_EQ(decoder.buffered_size(), bytes.size() - 1);

    decoder.append(bytes.data() + bytes.size() - 1, 1);
    ASSERT_TRUE(decoder.next_message(out));
    EXPECT_EQ(out.service_id, 0x1234u);
    EXPECT_EQ(out.payload, "AB");

    EXPECT_FALSE(decoder.next_message(out));
    EXPECT_EQ(decoder.buffered_size(), 0u);
}

TEST(SomeipDecoderTest, HandlesFragmentedInput)
{
    const std::string bytes = lab::someip::encode(make_response());
    lab::someip::Decoder decoder;
    Message out;

    // 逐字节喂入：直到最后一个字节之前都不应出报文
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        decoder.append(&bytes[i], 1);
        EXPECT_FALSE(decoder.next_message(out)) << "第 " << i << " 字节后不应出报文";
    }

    decoder.append(&bytes[bytes.size() - 1], 1);
    ASSERT_TRUE(decoder.next_message(out));
    EXPECT_EQ(out.payload, "AB");
    EXPECT_FALSE(decoder.next_message(out));
}

TEST(SomeipDecoderTest, DecodesBackToBackMessages)
{
    Message first = make_response();
    first.session_id = 1;
    Message second = make_response();
    second.session_id = 2;
    second.payload = "CD";

    const std::string bytes = lab::someip::encode(first) + lab::someip::encode(second);
    lab::someip::Decoder decoder;
    decoder.append(bytes);

    Message out;
    ASSERT_TRUE(decoder.next_message(out));
    EXPECT_EQ(out.session_id, 1u);
    EXPECT_EQ(out.payload, "AB");

    ASSERT_TRUE(decoder.next_message(out));
    EXPECT_EQ(out.session_id, 2u);
    EXPECT_EQ(out.payload, "CD");

    EXPECT_FALSE(decoder.next_message(out));
    EXPECT_EQ(decoder.buffered_size(), 0u);
}

TEST(SomeipDecoderTest, RejectsPayloadOverLimit)
{
    // 上限设成 32 字节，而报文声明了 100 字节载荷
    lab::someip::Decoder decoder(32);
    decoder.append(make_raw(108, 0));

    Message out;
    EXPECT_THROW(decoder.next_message(out), std::runtime_error);
}