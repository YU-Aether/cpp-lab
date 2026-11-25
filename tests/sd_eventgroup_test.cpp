#include "sd_codec.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using lab::someip::sd::Entry;
using lab::someip::sd::EntryType;
using lab::someip::sd::Message;

Entry make_eventgroup_entry(EntryType type)
{
    Entry entry;
    entry.type = type;
    entry.service_id = 0x1234;
    entry.instance_id = 0xFFFF;
    entry.major_version = 0x01;
    entry.ttl = 3;
    entry.eventgroup_id = 0x0010;
    return entry;
}

void expect_entry_bytes(const std::string& payload,
                        const std::vector<std::uint8_t>& expected)
{
    // entry 区固定从第 8 字节开始
    ASSERT_GE(payload.size(), 8 + expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(static_cast<std::uint8_t>(payload[8 + i]), expected[i])
            << "entry 字节 " << i;
    }
}

}  // namespace

TEST(SdEventgroupTest, EncodesSubscribeEventgroupExactBytes)
{
    Message msg;
    msg.entries.push_back(make_eventgroup_entry(EntryType::SubscribeEventgroup));

    const std::string payload = lab::someip::sd::encode_payload(msg);

    const std::vector<std::uint8_t> expected = {
        0x06, 0x00, 0x00, 0x00,  // Type=Subscribe, Index1, Index2, Counts
        0x12, 0x34,              // Service ID
        0xFF, 0xFF,              // Instance ID
        0x01,                    // Major Version
        0x00, 0x00, 0x03,        // TTL = 3
        0x00, 0x00,              // 保留
        0x00, 0x10,              // Eventgroup ID
    };
    expect_entry_bytes(payload, expected);
}

TEST(SdEventgroupTest, EncodesSubscribeEventgroupAckExactBytes)
{
    Message msg;
    msg.entries.push_back(make_eventgroup_entry(EntryType::SubscribeEventgroupAck));

    const std::string payload = lab::someip::sd::encode_payload(msg);

    const std::vector<std::uint8_t> expected = {
        0x07, 0x00, 0x00, 0x00,  // Type=Ack
        0x12, 0x34,
        0xFF, 0xFF,
        0x01,
        0x00, 0x00, 0x03,
        0x00, 0x00,
        0x00, 0x10,
    };
    expect_entry_bytes(payload, expected);
}

TEST(SdEventgroupTest, RoundTripSubscribeEventgroup)
{
    Message msg;
    msg.entries.push_back(make_eventgroup_entry(EntryType::SubscribeEventgroup));

    const Message decoded = lab::someip::sd::decode_payload(
        lab::someip::sd::encode_payload(msg));

    ASSERT_EQ(decoded.entries.size(), 1u);
    const Entry& entry = decoded.entries[0];
    EXPECT_EQ(entry.type, EntryType::SubscribeEventgroup);
    EXPECT_EQ(entry.service_id, 0x1234);
    EXPECT_EQ(entry.instance_id, 0xFFFF);
    EXPECT_EQ(entry.eventgroup_id, 0x0010);
    EXPECT_EQ(entry.ttl, 3u);
    // Type 2 不写 Minor Version
    EXPECT_EQ(entry.minor_version, 0u);
}

TEST(SdEventgroupTest, Type1StillCarriesMinorVersion)
{
    Entry entry;
    entry.type = EntryType::OfferService;
    entry.service_id = 0x1234;
    entry.instance_id = 0xFFFF;
    entry.major_version = 0x01;
    entry.ttl = 3;
    entry.minor_version = 0x00010203;

    Message msg;
    msg.entries.push_back(entry);

    const Message decoded = lab::someip::sd::decode_payload(
        lab::someip::sd::encode_payload(msg));

    ASSERT_EQ(decoded.entries.size(), 1u);
    EXPECT_EQ(decoded.entries[0].minor_version, 0x00010203u);
    // Type 1 不解释 Eventgroup ID
    EXPECT_EQ(decoded.entries[0].eventgroup_id, 0u);
}