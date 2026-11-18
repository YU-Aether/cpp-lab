#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace lab {

inline constexpr std::size_t kFrameHeaderSize = 4;
inline constexpr std::size_t kDefaultMaxFrameSize = 1024 * 1024;

// 按“4 字节大端长度 + 载荷”生成完整帧，长度只统计载荷字节数。
inline std::string encode_frame(const std::string& payload)
{
    std::string frame;
    frame.reserve(kFrameHeaderSize + payload.size());

    const auto length = static_cast<std::uint32_t>(payload.size());
    frame.push_back(static_cast<char>((length >> 24) & 0xFFu));
    frame.push_back(static_cast<char>((length >> 16) & 0xFFu));
    frame.push_back(static_cast<char>((length >> 8) & 0xFFu));
    frame.push_back(static_cast<char>(length & 0xFFu));

    frame.append(payload);
    return frame;
}

// 增量式分包解码器，可处理粘包、拆包与半包。
class FrameDecoder {
public:
    explicit FrameDecoder(std::size_t max_frame_size = kDefaultMaxFrameSize)
        : max_frame_size_(max_frame_size)
    {
    }

    void append(const char* data, std::size_t size)
    {
        buffer_.append(data, size);
    }

    void append(const std::string& data)
    {
        buffer_.append(data);
    }

    // 取出一帧，未凑齐返回 false，可反复调用直到返回 false。
    bool next_frame(std::string& out)
    {
        if (buffer_.size() < kFrameHeaderSize) {
            return false;
        }

        const std::size_t length = read_length();

        // 超长帧无法安全缓存，视为协议错误并清空缓冲。
        if (length > max_frame_size_) {
            buffer_.clear();
            throw std::runtime_error("帧长度超出允许上限");
        }

        if (buffer_.size() < kFrameHeaderSize + length) {
            return false;
        }

        // 长度允许为 0，此时得到的是空载荷的合法帧。
        out.assign(buffer_, kFrameHeaderSize, length);
        buffer_.erase(0, kFrameHeaderSize + length);
        return true;
    }

    std::size_t buffered_size() const
    {
        return buffer_.size();
    }

private:
    std::uint32_t read_length() const
    {
        // 先提升为无符号 32 位再移位，避免符号位参与移位产生未定义行为。
        const auto byte0 = static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(buffer_[0]));
        const auto byte1 = static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(buffer_[1]));
        const auto byte2 = static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(buffer_[2]));
        const auto byte3 = static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(buffer_[3]));

        return (byte0 << 24) | (byte1 << 16) | (byte2 << 8) | byte3;
    }

private:
    std::string buffer_;
    std::size_t max_frame_size_ = kDefaultMaxFrameSize;
};

}  // namespace lab