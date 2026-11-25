#include "sd_codec.h"
#include "sd_discovery.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace {

using lab::someip::sd::DiscoveredService;
using lab::someip::sd::SdDiscoverer;
using lab::someip::sd::SdResponder;

/// 把应答方跑在独立线程里，析构时保证 stop + join。
struct ResponderRunner {
    SdResponder& responder;
    std::thread thread;

    explicit ResponderRunner(SdResponder& target)
        : responder(target)
        , thread([this] { responder.run(); })
    {
    }

    ~ResponderRunner()
    {
        responder.stop();
        thread.join();
    }
};

/// 占住一个 UDP 端口但永不应答，用来构造"无人响应"的场景。
struct SilentUdpPort {
    int fd = -1;
    std::uint16_t port = 0;

    SilentUdpPort()
    {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            return;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            fd = -1;
            return;
        }

        socklen_t len = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
    }

    ~SilentUdpPort()
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

/// 往指定 UDP 端口发一段任意字节。
void send_udp(std::uint16_t port, const std::string& data)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    ::sendto(fd, data.data(), data.size(), 0,
             reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
}

}  // namespace

TEST(SdDiscoveryTest, ResponderRepliesWithOfferService)
{
    SdResponder responder(0, 9555);
    ResponderRunner runner(responder);

    SdDiscoverer discoverer;
    DiscoveredService service;
    const bool ok = discoverer.discover("127.0.0.1", responder.port(),
                                        std::chrono::milliseconds(1000), service);

    ASSERT_TRUE(ok);
    EXPECT_EQ(service.address, "127.0.0.1");
    EXPECT_EQ(service.port, 9555);
}

TEST(SdDiscoveryTest, DiscovererTimesOutWhenNoResponder)
{
    SilentUdpPort silent;

    SdDiscoverer discoverer;
    DiscoveredService service;
    const bool ok = discoverer.discover("127.0.0.1", silent.port,
                                        std::chrono::milliseconds(200), service);

    EXPECT_FALSE(ok);
}

TEST(SdDiscoveryTest, ResponderSurvivesGarbagePacket)
{
    SdResponder responder(0, 9555);
    ResponderRunner runner(responder);

    // 先丢一包完全不是 SOME/IP 的垃圾数据
    send_udp(responder.port(), "this-is-not-a-someip-frame");

    // 应答方不应该崩，后续正常发现仍然要成功
    SdDiscoverer discoverer;
    DiscoveredService service;
    const bool ok = discoverer.discover("127.0.0.1", responder.port(),
                                        std::chrono::milliseconds(1000), service);

    ASSERT_TRUE(ok);
    EXPECT_EQ(service.port, 9555);
}

TEST(SdDiscoveryTest, ResponderIgnoresUnknownService)
{
    SdResponder responder(0, 9555);
    ResponderRunner runner(responder);

    // 找一个客户端，但查询的是另一个服务号
    SdDiscoverer discoverer(0x9999);
    DiscoveredService service;
    const bool ok = discoverer.discover("127.0.0.1", responder.port(),
                                        std::chrono::milliseconds(200), service);

    EXPECT_FALSE(ok);
}