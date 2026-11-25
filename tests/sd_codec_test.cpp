#include "sd_codec.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using lab::someip::sd::Entry;
using lab::someip::sd::EntryType;
using lab::someip::sd::IPv4EndpointOption;
using lab::someip::sd::L4Protocol;
using lab::someip::sd::Message;

Entry make_find_service()
{
    Entry entry;
    entry.type = EntryType::FindService;
    entry.service_id = 0x1234;
    entry.instance_id = 0xFFFF;
    entry.major_version = 0x01;
    entry.ttl = 3;
    entry.minor_version = 0;
    return entry;
}

void expect_bytes(const std::string& actual,
                  const std::vector<std::uint8_t>& expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(static_cast<std::uint8_t>(actual[i]), expected[i])
            << "字节下标 " << i;
    }
}

}  // namespace

TEST(SdCodecTest, EncodesFindServiceExactBytes)
{
    Message msg;
    msg.entries.push_back(make_find_service());

    const std::string bytes = lab::someip::sd::encode_payload(msg);

    const std::vector<std::uint8_t> expected = {
        0x00, 0x00, 0x00, 0x00,  // Flags + Reserved
        0x00, 0x00, 0x00, 0x10,  // Entries Array Length = 16
        0x00, 0x00, 0x00, 0x00,  // Type=Find, Index1=0, Index2=0, Options=0|0
        0x12, 0x34,              // Service ID
        0xFF, 0xFF,              // Instance ID
        0x01,                    // Major Version
        0x00, 0x00, 0x03,        // TTL = 3
        0x00, 0x00, 0x00, 0x00,  // Minor Version
        0x00, 0x00, 0x00, 0x00,  // Options Array Length = 0
    };
    expect_bytes(bytes, expected);
}

TEST(SdCodecTest, EncodesOfferServiceWithEndpointOption)
{
    Message msg;
    Entry entry = make_find_service();
    entry.type = EntryType::OfferService;
    entry.num_options_first = 1;  // 第一组选项里有 1 个
    msg.entries.push_back(entry);

    IPv4EndpointOption option;
    option.address = {127, 0, 0, 1};
    option.protocol = L4Protocol::Tcp;
    option.port = 9555;
    msg.ipv4_options.push_back(option);

    const std::string bytes = lab::someip::sd::encode_payload(msg);

    const std::vector<std::uint8_t> expected = {
        0x00, 0x00, 0x00, 0x00,  // Flags + Reserved
        0x00, 0x00, 0x00, 0x10,  // Entries Array Length = 16
        0x01, 0x00, 0x00, 0x10,  // Type=Offer, Options: first=1, second=0
        0x12, 0x34,              // Service ID
        0xFF, 0xFF,              // Instance ID
        0x01,                    // Major Version
        0x00, 0x00, 0x03,        // TTL = 3
        0x00, 0x00, 0x00, 0x00,  // Minor Version
        0x00, 0x00, 0x00, 0x0B,  // Options Array Length = 11
        0x00, 0x09,              // Option Length = 9
        0x04,                    // Option Type = IPv4 Endpoint
        0x00,                    // Reserved
        0x7F, 0x00, 0x00, 0x01,  // 127.0.0.1
        0x06,                    // L4 Protocol = TCP
        0x25, 0x53,              // 端口 9555
    };
    expect_bytes(bytes, expected);
}

TEST(SdCodecTest, RoundTripFindService)
{
    Message msg;
    msg.entries.push_back(make_find_service());

    const Message decoded = lab::someip::sd::decode_payload(
        lab::someip::sd::encode_payload(msg));

    ASSERT_EQ(decoded.entries.size(), 1u);
    const Entry& entry = decoded.entries[0];
    EXPECT_EQ(entry.type, EntryType::FindService);
    EXPECT_EQ(entry.service_id, 0x1234);
    EXPECT_EQ(entry.instance_id, 0xFFFF);
    EXPECT_EQ(entry.major_version, 0x01);
    EXPECT_EQ(entry.ttl, 3u);
    EXPECT_EQ(entry.minor_version, 0u);
    EXPECT_TRUE(decoded.ipv4_options.empty());
}

TEST(SdCodecTest, RoundTripOfferService)
{
    Message msg;
    msg.flags = lab::someip::sd::kFlagReboot | lab::someip::sd::kFlagUnicast;

    Entry entry = make_find_service();
    entry.type = EntryType::OfferService;
    entry.num_options_first = 1;
    msg.entries.push_back(entry);

    IPv4EndpointOption option;
    option.address = {192, 168, 1, 10};
    option.protocol = L4Protocol::Udp;
    option.port = 30490;
    msg.ipv4_options.push_back(option);

    const Message decoded = lab::someip::sd::decode_payload(
        lab::someip::sd::encode_payload(msg));

    EXPECT_EQ(decoded.flags,
              lab::someip::sd::kFlagReboot | lab::someip::sd::kFlagUnicast);
    ASSERT_EQ(decoded.entries.size(), 1u);
    EXPECT_EQ(decoded.entries[0].type, EntryType::OfferService);
    ASSERT_EQ(decoded.ipv4_options.size(), 1u);
    EXPECT_EQ(decoded.ipv4_options[0].address, (std::array<std::uint8_t, 4>{192, 168, 1, 10}));
    EXPECT_EQ(decoded.ipv4_options[0].protocol, L4Protocol::Udp);
    EXPECT_EQ(decoded.ipv4_options[0].port, 30490);
}

TEST(SdCodecTest, PacksOptionCountsIntoOneByte)
{
    Message msg;
    Entry entry = make_find_service();
    entry.num_options_first = 2;
    entry.num_options_second = 3;
    msg.entries.push_back(entry);

    const std::string bytes = lab::someip::sd::encode_payload(msg);

    // 条目从第 8 字节开始，第 3 个字节是两个 4 位计数
    ASSERT_GT(bytes.size(), 11u);
    EXPECT_EQ(static_cast<std::uint8_t>(bytes[11]), 0x23);

    const Message decoded = lab::someip::sd::decode_payload(bytes);
    ASSERT_EQ(decoded.entries.size(), 1u);
    EXPECT_EQ(decoded.entries[0].num_options_first, 2);
    EXPECT_EQ(decoded.entries[0].num_options_second, 3);
}

TEST(SdCodecTest, RejectsTruncatedHeader)
{
    const std::string bytes(7, '\0');
    EXPECT_THROW(lab::someip::sd::decode_payload(bytes), std::runtime_error);
}

TEST(SdCodecTest, RejectsEntriesLengthNotMultipleOf16)
{
    std::string bytes(4, '\0');
    // Entries 长度 = 15
    bytes.append({0x00, 0x00, 0x00, 0x0F});
    EXPECT_THROW(lab::someip::sd::decode_payload(bytes), std::runtime_error);
}

TEST(SdCodecTest, RejectsUnknownEntryType)
{
    Message msg;
    Entry entry = make_find_service();
    entry.type = EntryType::FindService;
    msg.entries.push_back(entry);

    std::string bytes = lab::someip::sd::encode_payload(msg);
    bytes[8] = static_cast<char>(0x7F);  // 改成一个不认识的 entry 类型

    EXPECT_THROW(lab::someip::sd::decode_payload(bytes), std::runtime_error);
}

TEST(SdCodecTest, RejectsUnknownOptionType)
{
    Message msg;
    Entry entry = make_find_service();
    entry.type = EntryType::OfferService;
    entry.num_options_first = 1;
    msg.entries.push_back(entry);

    IPv4EndpointOption option;
    option.address = {127, 0, 0, 1};
    option.port = 9555;
    msg.ipv4_options.push_back(option);

    std::string bytes = lab::someip::sd::encode_payload(msg);
    // 选项从第 28 字节开始：长度(2) + 类型(1)
    bytes[30] = static_cast<char>(0x09);  // 改成不认识的选项类型

    EXPECT_THROW(lab::someip::sd::decode_payload(bytes), std::runtime_error);
}

TEST(SdCodecTest, RejectsTruncatedOptionsArray)
{
    Message msg;
    Entry entry = make_find_service();
    entry.type = EntryType::OfferService;
    entry.num_options_first = 1;
    msg.entries.push_back(entry);

    IPv4EndpointOption option;
    option.address = {127, 0, 0, 1};
    option.port = 9555;
    msg.ipv4_options.push_back(option);

    std::string bytes = lab::someip::sd::encode_payload(msg);
    bytes.resize(bytes.size() - 3);  // 砍掉选项尾部

    EXPECT_THROW(lab::someip::sd::decode_payload(bytes), std::runtime_error);
}

TEST(SdCodecTest, SdFrameUsesFixedServiceAndMethodIds)
{
    Message msg;
    msg.entries.push_back(make_find_service());

    const std::string frame = lab::someip::sd::encode_frame(msg);

    lab::someip::Decoder decoder;
    decoder.append(frame);
    lab::someip::Message outer;
    ASSERT_TRUE(decoder.next_message(outer));

    EXPECT_EQ(outer.service_id, 0xFFFF);
    EXPECT_EQ(outer.method_id, 0x8100);
    EXPECT_EQ(outer.message_type, lab::someip::MessageType::Notification);
    EXPECT_EQ(outer.interface_version, 0x01);
}

TEST(SdCodecTest, DecodesSdFrameFromSomeip)
{
    Message msg;
    Entry entry = make_find_service();
    entry.type = EntryType::OfferService;
    entry.num_options_first = 1;
    msg.entries.push_back(entry);

    IPv4EndpointOption option;
    option.address = {127, 0, 0, 1};
    option.port = 9555;
    msg.ipv4_options.push_back(option);

    const std::string frame = lab::someip::sd::encode_frame(msg);
    const lab::someip::Message outer = lab::someip::decode(frame);

    const Message decoded = lab::someip::sd::decode_payload(outer.payload);

    ASSERT_EQ(decoded.entries.size(), 1u);
    EXPECT_EQ(decoded.entries[0].type, EntryType::OfferService);
    ASSERT_EQ(decoded.ipv4_options.size(), 1u);
    EXPECT_EQ(decoded.ipv4_options[0].port, 9555);
}