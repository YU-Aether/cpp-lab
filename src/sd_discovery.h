#pragma once

#include "sd_codec.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
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
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

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

/// 取本机第一个非回环 IPv4 地址；找不到时退回 127.0.0.1。
inline std::string local_ipv4_address()
{
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return "127.0.0.1";
    }

    std::string result;
    for (ifaddrs* item = list; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((item->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        char text[INET_ADDRSTRLEN] = {};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        if (::inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) != nullptr) {
            result = text;
            break;
        }
    }

    ::freeifaddrs(list);
    return result.empty() ? "127.0.0.1" : result;
}

/// 发现结果。
struct DiscoveredService {
    std::string address;
    std::uint16_t port = 0;
};

/// 订阅事件组时使用的方法号，仅用于事件报文标识。
inline constexpr std::uint16_t kEventMethodId = 0x8001;

/// 服务端侧：响应 FindService、处理 SubscribeEventgroup、周期推送事件。
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

    /// 开启多播：加入指定组，并每隔 interval 主动广播一次 OfferService。
    void enable_multicast(const std::string& group,
                          std::chrono::milliseconds interval = std::chrono::milliseconds(1000))
    {
        multicast_address_ = sockaddr_in{};
        multicast_address_.sin_family = AF_INET;
        multicast_address_.sin_port = htons(port_);
        if (::inet_pton(AF_INET, group.c_str(), &multicast_address_.sin_addr) != 1) {
            throw std::runtime_error("多播组地址非法: " + group);
        }

        ip_mreq membership{};
        membership.imr_multiaddr = multicast_address_.sin_addr;
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::setsockopt(socket_.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &membership, sizeof(membership)) != 0) {
            throw std::runtime_error("加入多播组失败");
        }

        unsigned char loop = 1;
        unsigned char ttl = 1;
        ::setsockopt(socket_.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        ::setsockopt(socket_.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

        multicast_interval_ = interval;
        multicast_enabled_ = true;
    }

    /// 事件推送间隔。
    void set_event_interval(std::chrono::milliseconds interval)
    {
        event_interval_ = interval;
    }

    /// 阻塞运行，直到 stop() 被调用。
    void run()
    {
        auto next_broadcast = std::chrono::steady_clock::now();
        auto next_event = std::chrono::steady_clock::now();

        while (!stopped_.load(std::memory_order_relaxed)) {
            pollfd pfd{};
            pfd.fd = socket_.get();
            pfd.events = POLLIN;

            const int ready = ::poll(&pfd, 1, 50);
            const auto now = std::chrono::steady_clock::now();

            if (multicast_enabled_ && now >= next_broadcast) {
                broadcast_offer();
                next_broadcast = now + multicast_interval_;
            }

            if (!subscribers_.empty() && now >= next_event) {
                push_events();
                next_event = now + event_interval_;
            }

            if (ready <= 0 || (pfd.revents & POLLIN) == 0) {
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
            const Entry* subscribe_entry = nullptr;

            for (const Entry& entry : message.entries) {
                if (entry.service_id != service_id_) {
                    continue;
                }
                if (entry.type == EntryType::FindService) {
                    wants_offer = true;
                } else if (entry.type == EntryType::SubscribeEventgroup) {
                    subscribe_entry = &entry;
                }
            }

            if (wants_offer) {
                const std::string reply = build_offer();
                ::sendto(socket_.get(), reply.data(), reply.size(), 0,
                         reinterpret_cast<const sockaddr*>(&peer), peer_len);
            }

            if (subscribe_entry != nullptr) {
                handle_subscribe(message, *subscribe_entry, peer, peer_len);
            }
        } catch (const std::exception&) {
            // 垃圾包或畸形报文一律忽略
        }
    }

    void handle_subscribe(const Message& message, const Entry& entry,
                          const sockaddr_in& peer, socklen_t peer_len)
    {
        // 订阅者希望事件发到它自己的 UDP 端点，地址来自 IPv4 Endpoint 选项
        for (const IPv4EndpointOption& option : message.ipv4_options) {
            if (option.protocol != L4Protocol::Udp) {
                continue;
            }

            sockaddr_in subscriber{};
            subscriber.sin_family = AF_INET;
            std::memcpy(&subscriber.sin_addr.s_addr, option.address.data(), 4);
            subscriber.sin_port = htons(option.port);

            bool known = false;
            for (const sockaddr_in& existing : subscribers_) {
                if (existing.sin_addr.s_addr == subscriber.sin_addr.s_addr &&
                    existing.sin_port == subscriber.sin_port) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                subscribers_.push_back(subscriber);
            }

            const std::string reply = build_ack(entry.eventgroup_id);
            ::sendto(socket_.get(), reply.data(), reply.size(), 0,
                     reinterpret_cast<const sockaddr*>(&peer), peer_len);
            return;
        }
    }

    std::string build_offer() const
    {
        Message message;
        message.flags = kFlagUnicast;

        Entry entry;
        entry.type = EntryType::OfferService;
        entry.num_options_first = 1;
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

    std::string build_ack(std::uint16_t eventgroup_id) const
    {
        Message message;
        message.flags = kFlagUnicast;

        Entry entry;
        entry.type = EntryType::SubscribeEventgroupAck;
        entry.service_id = service_id_;
        entry.instance_id = instance_id_;
        entry.major_version = 0x01;
        entry.ttl = 3;
        entry.eventgroup_id = eventgroup_id;
        message.entries.push_back(entry);

        return encode_frame(message);
    }

    void broadcast_offer()
    {
        const std::string reply = build_offer();
        ::sendto(socket_.get(), reply.data(), reply.size(), 0,
                 reinterpret_cast<const sockaddr*>(&multicast_address_),
                 sizeof(multicast_address_));
    }

    void push_events()
    {
        someip::Message event;
        event.service_id = service_id_;
        event.method_id = kEventMethodId;
        event.client_id = 0x0000;
        event.session_id = static_cast<std::uint16_t>(++event_sequence_);
        event.interface_version = 0x01;
        event.message_type = someip::MessageType::Notification;
        event.return_code = someip::ReturnCode::Ok;
        event.payload = "EVENT-" + std::to_string(event_sequence_);

        const std::string frame = someip::encode(event);

        for (const sockaddr_in& subscriber : subscribers_) {
            ::sendto(socket_.get(), frame.data(), frame.size(), 0,
                     reinterpret_cast<const sockaddr*>(&subscriber),
                     sizeof(subscriber));
        }
    }

private:
    detail::UniqueFd socket_;
    std::atomic<bool> stopped_{false};
    std::uint16_t port_ = 0;
    std::uint16_t service_port_ = 0;
    std::uint16_t service_id_ = 0x1234;
    std::uint16_t instance_id_ = 0xFFFF;
    std::array<std::uint8_t, 4> advertised_address_{};

    bool multicast_enabled_ = false;
    sockaddr_in multicast_address_{};
    std::chrono::milliseconds multicast_interval_{1000};

    std::vector<sockaddr_in> subscribers_;
    std::chrono::milliseconds event_interval_{500};
    std::uint32_t event_sequence_ = 0;
};

/// 客户端侧：发 FindService 等待 OfferService。
class SdDiscoverer {
public:
    explicit SdDiscoverer(std::uint16_t service_id = 0x1234)
        : service_id_(service_id)
    {
    }

    /// sd_host 可以是单播地址，也可以是多播组地址。
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

        if (IN_MULTICAST(ntohl(target.sin_addr.s_addr)) != 0) {
            unsigned char loop = 1;
            unsigned char ttl = 1;
            ::setsockopt(socket.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
            ::setsockopt(socket.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
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

/// 订阅者：绑定本地 UDP 端口，发订阅请求并接收事件。
class SdSubscriber {
public:
    SdSubscriber(std::uint16_t service_id,
                 std::uint16_t instance_id,
                 std::uint16_t eventgroup_id)
        : service_id_(service_id)
        , instance_id_(instance_id)
        , eventgroup_id_(eventgroup_id)
    {
        socket_.reset(::socket(AF_INET, SOCK_DGRAM, 0));
        if (!socket_) {
            throw std::runtime_error("创建订阅 UDP socket 失败");
        }

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = 0;  // 由内核分配

        if (::bind(socket_.get(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            throw std::runtime_error("绑定订阅端口失败");
        }

        socklen_t len = sizeof(local);
        if (::getsockname(socket_.get(), reinterpret_cast<sockaddr*>(&local), &len) != 0) {
            throw std::runtime_error("获取订阅端口失败");
        }
        local_port_ = ntohs(local.sin_port);
    }

    SdSubscriber(const SdSubscriber&) = delete;
    SdSubscriber& operator=(const SdSubscriber&) = delete;

    std::uint16_t port() const { return local_port_; }

    /// 发送订阅请求并等待 Ack。
    bool subscribe(const std::string& sd_host, std::uint16_t sd_port,
                   std::chrono::milliseconds timeout)
    {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(sd_port);
        if (::inet_pton(AF_INET, sd_host.c_str(), &target.sin_addr) != 1) {
            return false;
        }

        const std::string frame = build_subscribe_request();
        if (::sendto(socket_.get(), frame.data(), frame.size(), 0,
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
            pfd.fd = socket_.get();
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
                continue;
            }

            const ssize_t n = ::recv(socket_.get(), buffer, sizeof(buffer), 0);
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
                    if (entry.type == EntryType::SubscribeEventgroupAck &&
                        entry.service_id == service_id_ &&
                        entry.eventgroup_id == eventgroup_id_) {
                        return true;
                    }
                }
            } catch (const std::exception&) {
                continue;
            }
        }
    }

    /// 等待一条事件，成功时把载荷写入 payload。
    bool receive_event(std::string& payload, std::chrono::milliseconds timeout)
    {
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
            pfd.fd = socket_.get();
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
                continue;
            }

            const ssize_t n = ::recv(socket_.get(), buffer, sizeof(buffer), 0);
            if (n <= 0) {
                continue;
            }

            try {
                someip::Decoder decoder;
                decoder.append(buffer, static_cast<std::size_t>(n));

                someip::Message message;
                if (!decoder.next_message(message)) {
                    continue;
                }
                if (message.service_id != service_id_) {
                    continue;
                }
                if (message.message_type != someip::MessageType::Notification) {
                    continue;
                }
                if (message.method_id != kEventMethodId) {
                    continue;
                }

                payload = message.payload;
                return true;
            } catch (const std::exception&) {
                continue;
            }
        }
    }

private:
    std::string build_subscribe_request() const
    {
        Message message;
        message.flags = kFlagUnicast;

        Entry entry;
        entry.type = EntryType::SubscribeEventgroup;
        entry.service_id = service_id_;
        entry.instance_id = instance_id_;
        entry.major_version = 0x01;
        entry.ttl = 3;
        entry.eventgroup_id = eventgroup_id_;
        entry.num_options_first = 1;  // 附带一个 IPv4 Endpoint 选项
        message.entries.push_back(entry);

        // 告诉服务端把事件发到本机的哪个 UDP 端口
        IPv4EndpointOption option;
        std::uint8_t bytes[4] = {};
        if (::inet_pton(AF_INET, local_ipv4_address().c_str(), bytes) == 1) {
            option.address = {bytes[0], bytes[1], bytes[2], bytes[3]};
        } else {
            option.address = {127, 0, 0, 1};
        }
        option.protocol = L4Protocol::Udp;
        option.port = local_port_;
        message.ipv4_options.push_back(option);

        return encode_frame(message);
    }

private:
    detail::UniqueFd socket_;
    std::uint16_t local_port_ = 0;
    std::uint16_t service_id_ = 0x1234;
    std::uint16_t instance_id_ = 0xFFFF;
    std::uint16_t eventgroup_id_ = 0x0001;
};

}  // namespace lab::someip::sd