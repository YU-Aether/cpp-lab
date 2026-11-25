#pragma once

#include "someip_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace lab::someip::sd {

/// SD 消息固定使用的 Service ID / Method ID。
inline constexpr std::uint16_t kServiceId = 0xFFFF;
inline constexpr std::uint16_t kMethodId = 0x8100;
inline constexpr std::uint8_t kInterfaceVersion = 0x01;

/// Flags 字节中的两位。
inline constexpr std::uint8_t kFlagReboot = 0x80;
inline constexpr std::uint8_t kFlagUnicast = 0x40;

/// 每个 entry 固定 16 字节。
inline constexpr std::size_t kEntrySize = 16;
/// IPv4 Endpoint 选项的类型码与 length 字段值。
inline constexpr std::uint8_t kIPv4EndpointOptionType = 0x04;
inline constexpr std::uint16_t kIPv4EndpointOptionLength = 9;

enum class EntryType : std::uint8_t {
    FindService = 0x00,
    OfferService = 0x01,
    SubscribeEventgroup = 0x06,
    SubscribeEventgroupAck = 0x07,
};

enum class L4Protocol : std::uint8_t {
    Tcp = 0x06,
    Udp = 0x11,
};

/// 服务发现条目。FindService / OfferService 共用这一种布局。
struct Entry {
    EntryType type = EntryType::FindService;
    std::uint8_t index_first_options = 0;
    std::uint8_t index_second_options = 0;
    std::uint8_t num_options_first = 0;
    std::uint8_t num_options_second = 0;
    std::uint16_t service_id = 0;
    std::uint16_t instance_id = 0;
    std::uint8_t major_version = 0;
    std::uint32_t ttl = 0;  // 仅低 24 位有效
    std::uint32_t minor_version = 0;
};

/// IPv4 Endpoint 选项：告诉对端这个服务实例的 IP、传输层协议和端口。
struct IPv4EndpointOption {
    std::array<std::uint8_t, 4> address{};
    L4Protocol protocol = L4Protocol::Tcp;
    std::uint16_t port = 0;
};

/// 一条完整的 SD 消息内容（不含外层 SOME/IP 头）。
struct Message {
    std::uint8_t flags = 0;
    std::vector<Entry> entries;
    std::vector<IPv4EndpointOption> ipv4_options;
};

namespace detail {

using ::lab::someip::detail::put_u16;
using ::lab::someip::detail::put_u32;
using ::lab::someip::detail::read_u16;
using ::lab::someip::detail::read_u32;

inline void put_u8(std::string& out, std::uint8_t value)
{
    out.push_back(static_cast<char>(value));
}

inline std::uint8_t read_u8(const char* data)
{
    return static_cast<std::uint8_t>(data[0]);
}

inline void put_u24(std::string& out, std::uint32_t value)
{
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

inline std::uint32_t read_u24(const char* data)
{
    const auto b0 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[2]));
    return (b0 << 16) | (b1 << 8) | b2;
}

inline bool is_known_entry_type(std::uint8_t value)
{
    switch (static_cast<EntryType>(value)) {
    case EntryType::FindService:
    case EntryType::OfferService:
    case EntryType::SubscribeEventgroup:
    case EntryType::SubscribeEventgroupAck:
        return true;
    }
    return false;
}

inline void encode_entry(std::string& out, const Entry& entry)
{
    put_u8(out, static_cast<std::uint8_t>(entry.type));
    put_u8(out, entry.index_first_options);
    put_u8(out, entry.index_second_options);
    // 两个 4 位计数打包在同一个字节：高 4 位第一组，低 4 位第二组
    put_u8(out, static_cast<std::uint8_t>(((entry.num_options_first & 0x0F) << 4) |
                                          (entry.num_options_second & 0x0F)));
    put_u16(out, entry.service_id);
    put_u16(out, entry.instance_id);
    put_u8(out, entry.major_version);
    put_u24(out, entry.ttl & 0x00FFFFFF);
    put_u32(out, entry.minor_version);
}

inline Entry decode_entry(const char* data)
{
    const std::uint8_t raw_type = read_u8(data);
    if (!is_known_entry_type(raw_type)) {
        throw std::runtime_error("SOME/IP-SD 未知条目类型");
    }

    Entry entry;
    entry.type = static_cast<EntryType>(raw_type);
    entry.index_first_options = read_u8(data + 1);
    entry.index_second_options = read_u8(data + 2);
    const std::uint8_t counts = read_u8(data + 3);
    entry.num_options_first = static_cast<std::uint8_t>((counts >> 4) & 0x0F);
    entry.num_options_second = static_cast<std::uint8_t>(counts & 0x0F);
    entry.service_id = read_u16(data + 4);
    entry.instance_id = read_u16(data + 6);
    entry.major_version = read_u8(data + 8);
    entry.ttl = read_u24(data + 9);
    entry.minor_version = read_u32(data + 12);
    return entry;
}

inline void encode_option(std::string& out, const IPv4EndpointOption& option)
{
    put_u16(out, kIPv4EndpointOptionLength);
    put_u8(out, kIPv4EndpointOptionType);
    put_u8(out, 0x00);  // reserved
    for (const std::uint8_t byte : option.address) {
        put_u8(out, byte);
    }
    put_u8(out, static_cast<std::uint8_t>(option.protocol));
    put_u16(out, option.port);
}

}  // namespace detail

/// 编码 SD 消息内容：Flags + 保留位 + 条目区 + 选项区。
inline std::string encode_payload(const Message& msg)
{
    std::string entries;
    for (const Entry& entry : msg.entries) {
        detail::encode_entry(entries, entry);
    }

    std::string options;
    for (const IPv4EndpointOption& option : msg.ipv4_options) {
        detail::encode_option(options, option);
    }

    std::string out;
    out.reserve(12 + entries.size() + options.size());

    detail::put_u8(out, msg.flags);
    detail::put_u8(out, 0x00);
    detail::put_u8(out, 0x00);
    detail::put_u8(out, 0x00);
    detail::put_u32(out, static_cast<std::uint32_t>(entries.size()));
    out += entries;
    detail::put_u32(out, static_cast<std::uint32_t>(options.size()));
    out += options;
    return out;
}

/// 解码 SD 消息内容；任何结构性错误都抛 std::runtime_error。
inline Message decode_payload(const std::string& payload)
{
    // Flags(1) + 保留(3) + 条目区长度(4)
    if (payload.size() < 8) {
        throw std::runtime_error("SOME/IP-SD 报文头不足");
    }

    Message msg;
    msg.flags = detail::read_u8(payload.data());

    const std::uint32_t entries_length = detail::read_u32(payload.data() + 4);
    if (entries_length % kEntrySize != 0) {
        throw std::runtime_error("SOME/IP-SD 条目区长度不是 16 的倍数");
    }

    const std::size_t entries_start = 8;
    if (payload.size() < entries_start + entries_length + 4) {
        throw std::runtime_error("SOME/IP-SD 条目区被截断");
    }

    for (std::size_t offset = 0; offset < entries_length; offset += kEntrySize) {
        msg.entries.push_back(detail::decode_entry(payload.data() + entries_start + offset));
    }

    const std::size_t options_length_offset = entries_start + entries_length;
    const std::uint32_t options_length =
        detail::read_u32(payload.data() + options_length_offset);

    const std::size_t options_start = options_length_offset + 4;
    if (payload.size() != options_start + options_length) {
        throw std::runtime_error("SOME/IP-SD 选项区长度与报文不符");
    }

    const char* options = payload.data() + options_start;
    std::size_t offset = 0;
    while (offset < options_length) {
        if (options_length - offset < 2) {
            throw std::runtime_error("SOME/IP-SD 选项长度字段被截断");
        }

        const std::uint16_t length = detail::read_u16(options + offset);
        if (length < 1 || options_length - offset < 2 + length) {
            throw std::runtime_error("SOME/IP-SD 选项长度非法");
        }

        if (detail::read_u8(options + offset + 2) != kIPv4EndpointOptionType) {
            throw std::runtime_error("SOME/IP-SD 未知选项类型");
        }
        if (length != kIPv4EndpointOptionLength) {
            throw std::runtime_error("SOME/IP-SD IPv4 选项长度非法");
        }

        // data[0] 保留，data[1..4] IPv4，data[5] 传输层协议，data[6..7] 端口
        const char* data = options + offset + 3;
        IPv4EndpointOption option;
        option.address = {static_cast<std::uint8_t>(data[1]),
                          static_cast<std::uint8_t>(data[2]),
                          static_cast<std::uint8_t>(data[3]),
                          static_cast<std::uint8_t>(data[4])};
        option.protocol = static_cast<L4Protocol>(static_cast<std::uint8_t>(data[5]));
        option.port = detail::read_u16(data + 6);
        msg.ipv4_options.push_back(option);

        offset += static_cast<std::size_t>(2) + length;
    }

    return msg;
}

/// 把 SD 消息包进一条完整的 SOME/IP 帧。
inline std::string encode_frame(const Message& msg, std::uint16_t session_id = 0)
{
    someip::Message outer;
    outer.service_id = kServiceId;
    outer.method_id = kMethodId;
    outer.client_id = 0x0000;
    outer.session_id = session_id;
    outer.protocol_version = someip::kProtocolVersion;
    outer.interface_version = kInterfaceVersion;
    outer.message_type = someip::MessageType::Notification;
    outer.return_code = someip::ReturnCode::Ok;
    outer.payload = encode_payload(msg);
    return someip::encode(outer);
}

}  // namespace lab::someip::sd