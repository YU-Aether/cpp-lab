#include "sd_codec.h"
#include "sd_discovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace {

using lab::someip::sd::DiscoveredService;
using lab::someip::sd::SdDiscoverer;
using lab::someip::sd::SdResponder;

// 独立用一个不常见的组和端口，避免和真实 30490 冲突
constexpr const char* kTestGroup = "239.255.0.1";
constexpr std::uint16_t kTestPort = 30999;

struct Runner {
    SdResponder& responder;
    std::thread thread;

    explicit Runner(SdResponder& target)
        : responder(target)
        , thread([this] { responder.run(); })
    {
    }

    ~Runner()
    {
        responder.stop();
        thread.join();
    }
};

}  // namespace

TEST(SdMulticastTest, DiscoversServiceThroughMulticastGroup)
{
    SdResponder responder(kTestPort, 9555);
    responder.enable_multicast(kTestGroup, std::chrono::milliseconds(200));
    Runner runner(responder);

    SdDiscoverer discoverer;
    DiscoveredService service;
    const bool ok = discoverer.discover(kTestGroup, kTestPort,
                                        std::chrono::milliseconds(1000), service);

    ASSERT_TRUE(ok);
    // 构造时没有指定通告地址，默认是 127.0.0.1
    EXPECT_EQ(service.address, "127.0.0.1");
    EXPECT_EQ(service.port, 9555);
}
