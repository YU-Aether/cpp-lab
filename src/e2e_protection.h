#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lab::e2e {

/// E2E 头长度：Counter(2) + Data ID(4) + CRC(4)
inline constexpr std::size_t kHeaderSize = 10;

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

}  // namespace detail

/// 一条需要 E2E 保护的数据帧。
struct Frame {
    std::uint16_t counter = 0;
    std::uint32_t data_id = 0;
    std::string data;
};

/// CRC-32（IEEE 802.3，反射多项式 0xEDB88320）。
///
/// AUTOSAR Profile 4 用的是 CRC32P4（多项式 0xF4ACFB13），
/// 若要对齐规范，只需把这里的多项式替换掉，调用方无需改动。
inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t crc = 0xFFFFFFFFu;

    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = (crc & 1u) != 0 ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

inline std::uint32_t crc32(const std::string& data)
{
    return crc32(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

/// 给数据加上 E2E 头，CRC 覆盖 Counter + Data ID + 数据本身。
inline std::string protect(const Frame& frame)
{
    // 先拼出被 CRC 覆盖的部分
    std::string body;
    body.reserve(6 + frame.data.size());
    detail::put_u16(body, frame.counter);
    detail::put_u32(body, frame.data_id);
    body += frame.data;

    const std::uint32_t checksum = crc32(body);

    std::string out;
    out.reserve(kHeaderSize + frame.data.size());
    out.append(body, 0, 6);                      // Counter + Data ID
    detail::put_u32(out, checksum);              // CRC
    out.append(body, 6, std::string::npos);      // 数据
    return out;
}

/// 校验并拆解；CRC 不符或长度不足返回 false。
inline bool unprotect(const std::string& bytes, Frame& out)
{
    if (bytes.size() < kHeaderSize) {
        return false;
    }

    const std::uint16_t counter = detail::read_u16(bytes.data());
    const std::uint32_t data_id = detail::read_u32(bytes.data() + 2);
    const std::uint32_t expected = detail::read_u32(bytes.data() + 6);

    // 重算时同样跳过 CRC 字段本身
    std::string body;
    body.reserve(6 + (bytes.size() - kHeaderSize));
    body.append(bytes, 0, 6);
    body.append(bytes, kHeaderSize, std::string::npos);

    if (crc32(body) != expected) {
        return false;
    }

    out.counter = counter;
    out.data_id = data_id;
    out.data.assign(bytes, kHeaderSize, std::string::npos);
    return true;
}

}  // namespace lab::e2e