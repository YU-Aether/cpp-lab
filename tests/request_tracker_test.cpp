#include "request_tracker.h"

#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>

namespace {

using Clock = std::chrono::steady_clock;
using lab::RequestTracker;

/// 以毫秒构造一个固定时间点，便于精确推进。
Clock::time_point at(int ms)
{
    return Clock::time_point{} + std::chrono::milliseconds(ms);
}

}  // namespace

TEST(RequestTrackerTest, TickBeforeDeadlineDoesNothing)
{
    RequestTracker tracker(std::chrono::milliseconds(200), 2);
    tracker.track(1, "payload", at(0));

    const auto result = tracker.tick(at(199));
    EXPECT_TRUE(result.retry_sessions.empty());
    EXPECT_TRUE(result.timed_out_sessions.empty());
    EXPECT_EQ(tracker.size(), 1u);
}

TEST(RequestTrackerTest, TickAfterDeadlineSchedulesRetry)
{
    RequestTracker tracker(std::chrono::milliseconds(200), 2);
    tracker.track(7, "payload", at(0));

    const auto result = tracker.tick(at(200));
    ASSERT_EQ(result.retry_sessions.size(), 1u);
    EXPECT_EQ(result.retry_sessions[0], 7);
    EXPECT_TRUE(result.timed_out_sessions.empty());
    EXPECT_EQ(tracker.payload_of(7), "payload");
}

TEST(RequestTrackerTest, RetryResetsDeadline)
{
    RequestTracker tracker(std::chrono::milliseconds(200), 2);
    tracker.track(3, "p", at(0));

    // 第一次超时触发重发，新的 deadline 推到 400
    const auto first = tracker.tick(at(200));
    ASSERT_EQ(first.retry_sessions.size(), 1u);

    // 刚重发完不应立刻再次触发
    const auto second = tracker.tick(at(399));
    EXPECT_TRUE(second.retry_sessions.empty());
    EXPECT_TRUE(second.timed_out_sessions.empty());
}

TEST(RequestTrackerTest, ReportsTimeoutAfterMaxRetries)
{
    RequestTracker tracker(std::chrono::milliseconds(100), 1);
    tracker.track(5, "p", at(0));

    // 第一次超时：还有重试额度，重发
    const auto first = tracker.tick(at(100));
    ASSERT_EQ(first.retry_sessions.size(), 1u);
    EXPECT_TRUE(first.timed_out_sessions.empty());

    // 第二次超时：额度已用完，彻底放弃
    const auto second = tracker.tick(at(200));
    ASSERT_EQ(second.timed_out_sessions.size(), 1u);
    EXPECT_EQ(second.timed_out_sessions[0], 5);
    EXPECT_EQ(tracker.size(), 0u);
}

TEST(RequestTrackerTest, AcknowledgeRemovesPending)
{
    RequestTracker tracker(std::chrono::milliseconds(200), 2);
    tracker.track(9, "p", at(0));

    EXPECT_TRUE(tracker.acknowledge(9));
    EXPECT_EQ(tracker.size(), 0u);

    // 同 session 的重复响应：已经不在表里
    EXPECT_FALSE(tracker.acknowledge(9));
}

TEST(RequestTrackerTest, AcknowledgeUnknownReturnsFalse)
{
    RequestTracker tracker;
    EXPECT_FALSE(tracker.acknowledge(1234));
}

TEST(RequestTrackerTest, ClearRemovesEverything)
{
    RequestTracker tracker;
    tracker.track(1, "a", at(0));
    tracker.track(2, "b", at(0));
    ASSERT_EQ(tracker.size(), 2u);

    tracker.clear();
    EXPECT_EQ(tracker.size(), 0u);
    EXPECT_FALSE(tracker.contains(1));
    EXPECT_FALSE(tracker.contains(2));
}