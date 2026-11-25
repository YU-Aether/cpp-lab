#include "sd_codec.h"
#include "sd_discovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace {

using lab::someip::sd::SdResponder;
using lab::someip::sd::SdSubscriber;

constexpr std::uint16_t kSdPort = 30997;
constexpr std::uint16_t kServiceId = 0x1234;
constexpr std::uint16_t kInstanceId = 0xFFFF;
constexpr std::uint16_t kEventgroupId = 0x0010;

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

TEST(SdSubscriptionTest, ResponderAcksSubscription)
{
    SdResponder responder(kSdPort, 9555);
    responder.set_event_interval(std::chrono::milliseconds(200));
    Runner runner(responder);

    SdSubscriber subscriber(kServiceId, kInstanceId, kEventgroupId);
    EXPECT_TRUE(subscriber.subscribe("127.0.0.1", kSdPort,
                                     std::chrono::milliseconds(1000)));
}

TEST(SdSubscriptionTest, PushesEventsAfterSubscription)
{
    SdResponder responder(kSdPort, 9555);
    responder.set_event_interval(std::chrono::milliseconds(200));
    Runner runner(responder);

    SdSubscriber subscriber(kServiceId, kInstanceId, kEventgroupId);
    ASSERT_TRUE(subscriber.subscribe("127.0.0.1", kSdPort,
                                     std::chrono::milliseconds(1000)));

    std::string payload;
    ASSERT_TRUE(subscriber.receive_event(payload, std::chrono::milliseconds(2000)));
    EXPECT_EQ(payload.rfind("EVENT-", 0), 0u);
}

TEST(SdSubscriptionTest, DeliversMultipleEvents)
{
    SdResponder responder(kSdPort, 9555);
    responder.set_event_interval(std::chrono::milliseconds(100));
    Runner runner(responder);

    SdSubscriber subscriber(kServiceId, kInstanceId, kEventgroupId);
    ASSERT_TRUE(subscriber.subscribe("127.0.0.1", kSdPort,
                                     std::chrono::milliseconds(1000)));

    std::string first;
    ASSERT_TRUE(subscriber.receive_event(first, std::chrono::milliseconds(2000)));

    std::string second;
    ASSERT_TRUE(subscriber.receive_event(second, std::chrono::milliseconds(2000)));

    EXPECT_NE(first, second);
}