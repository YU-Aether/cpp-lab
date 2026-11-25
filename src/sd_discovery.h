#pragma once

#include "sd_codec.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace lab::someip::sd {

namespace detail {

/// 独占 fd，析构自动 close。
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}

    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const { return fd_; }

    void reset(int fd = -1)
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

    int release()
    {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

}  // namespace detail

/// 发现结果：服务实例的 IP 与 TCP 端口。
struct DiscoveredService {
    std::string address;
    std::uint16_t port = 0;
};

/// 服务端侧：监听 UDP，收到 FindService 就单播回 OfferService。
///
/// 这是简化版发现：不做多播，客户端需要预先知道 SD 端口，
/// 但业务端口完全由 OfferService 动态告知。
class SdResponder {
public:
    explicit SdResponder(std::uint16_t sd_port,
                         std::uint16_t service_port,
                         std::uint16_t service_id = 0x1234,
                         std::uint16_t instance_id = 0xFFFF,
                         const std::string& advertised_address = "127.0.0.1")
        : socket_(::socket(AF_INET, SOCK_DGRAM, 0))
        , service_port_(service_port)
        , service_id_(service_id)
        , instance_id_(instance_id)
    {
        if (!socket_) {
            throw std::runtime_error("创建 SD UDP socket 失败");
        }

        int reuse = 1;
        ::setsockopt(socket_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(sd_port);

        if (::bind(socket_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("绑定 SD 端口失败");
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(socket_.get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error("获取 SD 端口失败");
        }
        port_ = ntohs(addr.sin_port);

        if (::inet_pton(AF_INET, advertised_address.c_str(),
                        advertised_address_.data()) != 1) {
            throw std::runtime_error("通告地址非法: " + advertised_address);
        }
    }

    SdResponder(const SdResponder&) = delete;
    SdResponder& operator=(const SdResponder&) = delete;

    ~SdResponder() { stop(); }

    std::uint16_t port() const { return port_; }

    /// 阻塞运行，直到 stop() 被调用。
    void run()
    {
        while (!stopped_.load(std::memory_order_relaxed)) {
            pollfd pfd{};
            pfd.fd = socket_.get();
            pfd.events = POLLIN;

            if (::poll(&pfd, 1, 50) <= 0) {
                continue;
            }

            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            char buffer[2048];

            const ssize_t n = ::recvfrom(socket_.get(), buffer, sizeof(buffer), 0,
                                         reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (n <= 0) {
                continue;
            }

            handle_datagram(buffer, static_cast<std::size_t>(n), peer, peer_len);
        }
    }

    void stop() { stopped_.store(true); }

private:
    void handle_datagram(const char* data, std::size_t size,
                         const sockaddr_in& peer, socklen_t peer_len)
    {
        try {
            someip::Decoder decoder;
            decoder.append(data, size);

            someip::Message outer;
            if (!decoder.next_message(outer)) {
                return;
            }
            if (outer.service_id != kServiceId || outer.method_id != kMethodId) {
                return;
            }

            const Message message = decode_payload(outer.payload);

            bool wants_offer = false;
            for (const Entry& entry : message.entries) {
                if (entry.type == EntryType::FindService &&
                    entry.service_id == service_id_) {
                    wants_offer = true;
                    break;
                }
            }
            if (!wants_offer) {
                return;
            }

            const std::string reply = build_offer();
            ::sendto(socket_.get(), reply.data(), reply.size(), 0,
                     reinterpret_cast<const sockaddr*>(&peer), peer_len);
        } catch (const std::exception&) {
            // 垃圾包或畸形报文一律忽略，不能让一个坏包打断整个服务
        }
    }

    std::string build_offer() const
    {
        Message message;
        message.flags = kFlagUnicast;

        Entry entry;
        entry.type = EntryType::OfferService;
        entry.num_options_first = 1;  // 只有一个 IPv4 Endpoint 选项
        entry.service_id = service_id_;
        entry.instance_id = instance_id_;
        entry.major_version = 0x01;
        entry.ttl = 3;
        entry.minor_version = 0;
        message.entries.push_back(entry);

        IPv4EndpointOption option;
        option.address = advertised_address_;
        option.protocol = L4Protocol::Tcp;
        option.port = service_port_;
        message.ipv4_options.push_back(option);

        return encode_frame(message);
    }

private:
    detail::UniqueFd socket_;
    std::atomic<bool> stopped_{false};
    std::uint16_t port_ = 0;
    std::uint16_t service_port_ = 0;
    std::uint16_t service_id_ = 0x1234;
    std::uint16_t instance_id_ = 0xFFFF;
    std::array<std::uint8_t, 4> advertised_address_{};
};

/// 客户端侧：发 FindService，等待 OfferService，解析出 IP + 端口。
class SdDiscoverer {
public:
    explicit SdDiscoverer(std::uint16_t service_id = 0x1234)
        : service_id_(service_id)
    {
    }

    /// 成功返回 true 并填充 out；超时或报文非法返回 false。
    bool discover(const std::string& sd_host, std::uint16_t sd_port,
                  std::chrono::milliseconds timeout, DiscoveredService& out)
    {
        detail::UniqueFd socket(::socket(AF_INET, SOCK_DGRAM, 0));
        if (!socket) {
            return false;
        }

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(sd_port);
        if (::inet_pton(AF_INET, sd_host.c_str(), &target.sin_addr) != 1) {
            return false;
        }

        const std::string frame = build_find_request();
        if (::sendto(socket.get(), frame.data(), frame.size(), 0,
                     reinterpret_cast<sockaddr*>(&target), sizeof(target)) < 0) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        char buffer[2048];

        while (true) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0) {
                return false;
            }

            pollfd pfd{};
            pfd.fd = socket.get();
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
                continue;
            }

            const ssize_t n = ::recv(socket.get(), buffer, sizeof(buffer), 0);
            if (n <= 0) {
                continue;
            }

            try {
                someip::Decoder decoder;
                decoder.append(buffer, static_cast<std::size_t>(n));

                someip::Message outer;
                if (!decoder.next_message(outer)) {
                    continue;
                }
                if (outer.service_id != kServiceId || outer.method_id != kMethodId) {
                    continue;
                }

                const Message message = decode_payload(outer.payload);
                for (const Entry& entry : message.entries) {
                    if (entry.type != EntryType::OfferService) {
                        continue;
                    }
                    if (entry.service_id != service_id_) {
                        continue;
                    }
                    if (message.ipv4_options.empty()) {
                        continue;
                    }

                    const IPv4EndpointOption& option = message.ipv4_options[0];
                    char text[INET_ADDRSTRLEN] = {};
                    if (::inet_ntop(AF_INET, option.address.data(), text,
                                    sizeof(text)) == nullptr) {
                        continue;
                    }

                    out.address = text;
                    out.port = option.port;
                    return true;
                }
            } catch (const std::exception&) {
                continue;
            }
        }
    }

private:
    std::string build_find_request() const
    {
        Message message;
        message.flags = kFlagUnicast;

        Entry entry;
        entry.type = EntryType::FindService;
        entry.service_id = service_id_;
        entry.instance_id = 0xFFFF;
        entry.major_version = 0x01;
        entry.ttl = 3;
        entry.minor_version = 0;
        message.entries.push_back(entry);

        return encode_frame(message);
    }

private:
    std::uint16_t service_id_ = 0x1234;
};

}  // namespace lab::someip::sd