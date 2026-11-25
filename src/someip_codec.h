#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace lab::someip {

/// SOME/IP 固定头部长度。
inline constexpr std::size_t kHeaderSize = 16;
/// 协议版本固定为 1，解码时校验。
inline constexpr std::uint8_t kProtocolVersion = 0x01;
/// Length 字段的最小值：从 Request ID 起算，至少 8 字节。
inline constexpr std::size_t kMinLengthField = 8;
/// 默认载荷上限，防止畸形长度字段把内存吃光。
inline constexpr std::size_t kDefaultMaxPayloadSize = 1024 * 1024;

/// 消息类型。
enum class MessageType : std::uint8_t {
    Request = 0x00,
    RequestNoReturn = 0x01,
    Notification = 0x02,
    Response = 0x80,
    Error = 0x81,
};

/// 返回码。
enum class ReturnCode : std::uint8_t {
    Ok = 0x00,
    NotOk = 0x01,
    UnknownService = 0x02,
    UnknownMethod = 0x03,
    NotReady = 0x04,
    NotReachable = 0x05,
    Timeout = 0x06,
    WrongProtocolVersion = 0x07,
    WrongInterfaceVersion = 0x08,
    MalformedMessage = 0x09,
    WrongMessageType = 0x0A,
};

/// 一条完整的 SOME/IP 报文（所有多字节字段均已解析成大端值）。
struct Message {
    std::uint16_t service_id = 0;
    std::uint16_t method_id = 0;
    std::uint16_t client_id = 0;
    std::uint16_t session_id = 0;
    std::uint8_t protocol_version = kProtocolVersion;
    std::uint8_t interface_version = 0;
    MessageType message_type = MessageType::Request;
    ReturnCode return_code = ReturnCode::Ok;
    std::string payload;
};

/// 协议错误。异常里带上已解析出的头部，
/// 服务端才能在"报文不可信"时依然回一条带正确标识的 Error。
class ProtocolError : public std::runtime_error {
public:
    enum class Reason {
        Truncated,       // 头部不足 16 字节
        BadLength,       // 长度字段非法或与实际字节数不符
        TooLarge,        // 载荷超过允许上限
        VersionMismatch, // 协议版本不是 1
    };

    ProtocolError(Reason reason, const std::string& message, Message header = {})
        : std::runtime_error(message)
        , reason_(reason)
        , header_(std::move(header))
    {
    }

    Reason reason() const noexcept { return reason_; }
    const Message& header() const noexcept { return header_; }

private:
    Reason reason_;
    Message header_;
};

namespace detail {

inline void put_u16(std::string& out, std::uint16_t value)
{
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

inline void put_u32(std::string& out, std::uint32_t value)
{
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

inline std::uint16_t read_u16(const char* data)
{
    const auto hi = static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[0]));
    const auto lo = static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[1]));
    return static_cast<std::uint16_t>((hi << 8) | lo);
}

inline std::uint32_t read_u32(const char* data)
{
    const auto b0 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[2]));
    const auto b3 = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[3]));
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

/// 只读头部字段，不做任何校验。调用方必须保证已有 16 字节。
inline Message read_header(const char* data)
{
    Message msg;
    msg.service_id = read_u16(data);
    msg.method_id = read_u16(data + 2);
    msg.client_id = read_u16(data + 8);
    msg.session_id = read_u16(data + 10);
    msg.protocol_version = static_cast<std::uint8_t>(data[12]);
    msg.interface_version = static_cast<std::uint8_t>(data[13]);
    msg.message_type = static_cast<MessageType>(static_cast<std::uint8_t>(data[14]));
    msg.return_code = static_cast<ReturnCode>(static_cast<std::uint8_t>(data[15]));
    return msg;
}

/// 解析一块长度已经确认吻合的数据（头部 + 载荷）。
inline Message parse(const char* data, std::size_t size)
{
    if (size < kHeaderSize) {
        throw ProtocolError(ProtocolError::Reason::Truncated,
                            "SOME/IP 报文不足 16 字节头部");
    }

    const Message header = read_header(data);

    const std::uint32_t length_field = read_u32(data + 4);
    if (length_field < kMinLengthField) {
        throw ProtocolError(ProtocolError::Reason::BadLength,
                            "SOME/IP 长度字段小于 8", header);
    }

    const std::size_t payload_size = length_field - kMinLengthField;
    if (size != kHeaderSize + payload_size) {
        throw ProtocolError(ProtocolError::Reason::BadLength,
                            "SOME/IP 报文长度与长度字段不符", header);
    }

    if (header.protocol_version != kProtocolVersion) {
        throw ProtocolError(ProtocolError::Reason::VersionMismatch,
                            "SOME/IP 协议版本不匹配", header);
    }

    Message msg = header;
    msg.payload.assign(data + kHeaderSize, payload_size);
    return msg;
}

}  // namespace detail

/// 编码一条完整报文：16 字节头 + 载荷，全部大端。
inline std::string encode(const Message& msg)
{
    std::string out;
    out.reserve(kHeaderSize + msg.payload.size());

    detail::put_u16(out, msg.service_id);
    detail::put_u16(out, msg.method_id);
    // Length 覆盖 Request ID 到报文末尾，因此等于 8 + 载荷长度
    detail::put_u32(out, static_cast<std::uint32_t>(kMinLengthField + msg.payload.size()));
    detail::put_u16(out, msg.client_id);
    detail::put_u16(out, msg.session_id);
    out.push_back(static_cast<char>(msg.protocol_version));
    out.push_back(static_cast<char>(msg.interface_version));
    out.push_back(static_cast<char>(msg.message_type));
    out.push_back(static_cast<char>(msg.return_code));
    out.append(msg.payload);
    return out;
}

/// 解码一条完整报文；长度不是刚好吻合就视为协议错误。
inline Message decode(const std::string& data)
{
    return detail::parse(data.data(), data.size());
}

/// 流式解码器：解决 TCP 上的粘包/半包，语义与 FrameDecoder 一致。
class Decoder {
public:
    explicit Decoder(std::size_t max_payload_size = kDefaultMaxPayloadSize)
        : max_payload_size_(max_payload_size)
    {
    }

    void append(const char* data, std::size_t size) { buffer_.append(data, size); }
    void append(const std::string& data) { buffer_.append(data); }

    /// 取出一条完整报文。数据不足返回 false；协议错误抛 ProtocolError。
    bool next_message(Message& out)
    {
        if (buffer_.size() < kHeaderSize) {
            return false;
        }

        const std::uint32_t length_field = detail::read_u32(buffer_.data() + 4);
        if (length_field < kMinLengthField) {
            const Message header = detail::read_header(buffer_.data());
            buffer_.clear();
            throw ProtocolError(ProtocolError::Reason::BadLength,
                                "SOME/IP 长度字段小于 8", header);
        }

        const std::size_t payload_size = length_field - kMinLengthField;
        if (payload_size > max_payload_size_) {
            const Message header = detail::read_header(buffer_.data());
            buffer_.clear();
            throw ProtocolError(ProtocolError::Reason::TooLarge,
                                "SOME/IP 载荷超出允许上限", header);
        }

        const std::size_t total = kHeaderSize + payload_size;
        if (buffer_.size() < total) {
            return false;
        }

        out = detail::parse(buffer_.data(), total);
        // 与 FrameDecoder 相同的取舍：从头部搬移，帧少时够用，
        // 高吞吐场景应换成环形缓冲区
        buffer_.erase(0, total);
        return true;
    }

    std::size_t buffered_size() const { return buffer_.size(); }

private:
    std::string buffer_;
    std::size_t max_payload_size_;
};

}  // namespace lab::someip