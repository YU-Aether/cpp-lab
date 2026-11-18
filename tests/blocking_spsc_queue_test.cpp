#include "blocking_spsc_queue.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

TEST(BlockingSPSCQueueTest, TryOperationsKeepFifoOrder)
{
    BlockingSPSCQueue<int, 3> queue;

    int out = -1;
    EXPECT_FALSE(queue.try_pop(out));
    EXPECT_EQ(out, -1);
    EXPECT_EQ(queue.size_approx(), 0u);

    int one = 1;
    EXPECT_TRUE(queue.try_push(one));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_TRUE(queue.try_push(3));
    EXPECT_FALSE(queue.try_push(4));
    EXPECT_EQ(queue.size_approx(), 3u);

    for (int expected = 1; expected <= 3; ++expected)
    {
        EXPECT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, expected);
    }

    EXPECT_EQ(queue.size_approx(), 0u);
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(BlockingSPSCQueueTest, BlockingPopWaitsForTryPush)
{
    BlockingSPSCQueue<int, 1> queue;

    std::promise<void> consumer_entered_promise;
    auto consumer_entered = consumer_entered_promise.get_future();

    int out = 0;
    auto consumer = std::async(std::launch::async, [&] {
        consumer_entered_promise.set_value();
        queue.pop(out);
    });

    // 确认消费者已经进入 pop，再检查它是否保持等待。
    EXPECT_EQ(consumer_entered.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(consumer.wait_for(50ms), std::future_status::timeout);

    // try_push 同样必须唤醒阻塞中的消费者。
    EXPECT_TRUE(queue.try_push(42));
    EXPECT_EQ(consumer.wait_for(1s), std::future_status::ready);

    consumer.get();
    EXPECT_EQ(out, 42);
    EXPECT_EQ(queue.size_approx(), 0u);
}

TEST(BlockingSPSCQueueTest, BlockingPushWaitsForTryPop)
{
    BlockingSPSCQueue<int, 1> queue;
    queue.push(1);

    std::promise<void> producer_entered_promise;
    auto producer_entered = producer_entered_promise.get_future();

    auto producer = std::async(std::launch::async, [&] {
        producer_entered_promise.set_value();
        queue.push(2);
    });

    // 队列已满，第二个生产者必须等待消费者腾出空间。
    EXPECT_EQ(producer_entered.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(producer.wait_for(50ms), std::future_status::timeout);

    int first = 0;
    EXPECT_TRUE(queue.try_pop(first));
    EXPECT_EQ(first, 1);

    // try_pop 同样必须唤醒阻塞中的生产者。
    EXPECT_EQ(producer.wait_for(1s), std::future_status::ready);
    producer.get();

    int second = 0;
    EXPECT_TRUE(queue.try_pop(second));
    EXPECT_EQ(second, 2);
}

TEST(BlockingSPSCQueueTest, PopForTimesOutOnEmptyQueue)
{
    BlockingSPSCQueue<int, 2> queue;

    int out = 7;
    const auto start = std::chrono::steady_clock::now();
    const bool success = queue.pop_for(out, 50ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(success);
    EXPECT_EQ(out, 7);
    EXPECT_EQ(queue.size_approx(), 0u);

    // 留出调度误差，但返回值不能早于设定的超时时间太多。
    EXPECT_GE(elapsed, 30ms);
}

TEST(BlockingSPSCQueueTest, PushForTimesOutOnFullQueue)
{
    BlockingSPSCQueue<int, 1> queue;
    queue.push(1);

    int value = 2;
    const auto start = std::chrono::steady_clock::now();
    const bool success = queue.push_for(value, 50ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(success);
    EXPECT_EQ(value, 2);
    EXPECT_EQ(queue.size_approx(), 1u);
    EXPECT_GE(elapsed, 30ms);

    // 超时失败不能破坏队列中已有的数据。
    int out = 0;
    EXPECT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 1);
}

TEST(BlockingSPSCQueueTest, PopForSucceedsWhenValueArrives)
{
    BlockingSPSCQueue<int, 1> queue;

    auto producer = std::async(std::launch::async, [&] {
        std::this_thread::sleep_for(50ms);

        int value = 99;
        queue.push(value);
    });

    int out = 0;
    EXPECT_TRUE(queue.pop_for(out, 2s));
    EXPECT_EQ(out, 99);

    producer.get();
    EXPECT_EQ(queue.size_approx(), 0u);
}

TEST(BlockingSPSCQueueTest, PushForSucceedsWhenSpaceArrives)
{
    BlockingSPSCQueue<int, 1> queue;
    queue.push(1);

    int first = 0;
    auto consumer = std::async(std::launch::async, [&] {
        std::this_thread::sleep_for(50ms);
        queue.pop(first);
    });

    EXPECT_TRUE(queue.push_for(2, 2s));

    consumer.get();
    EXPECT_EQ(first, 1);

    int second = 0;
    EXPECT_TRUE(queue.try_pop(second));
    EXPECT_EQ(second, 2);
}

TEST(BlockingSPSCQueueTest, SupportsMoveOnlyValues)
{
    BlockingSPSCQueue<std::unique_ptr<int>, 2> queue;

    EXPECT_TRUE(queue.try_push(std::make_unique<int>(10)));
    queue.push(std::make_unique<int>(20));

    std::unique_ptr<int> first;
    queue.pop(first);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(*first, 10);

    EXPECT_TRUE(queue.push_for(std::make_unique<int>(30), 50ms));

    std::unique_ptr<int> second;
    EXPECT_TRUE(queue.pop_for(second, 50ms));
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(*second, 20);

    std::unique_ptr<int> third;
    queue.pop(third);
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(*third, 30);
}

TEST(BlockingSPSCQueueTest, ConcurrentTransferPreservesOrder)
{
    constexpr int count = 100000;
    BlockingSPSCQueue<int, 64> queue;

    auto producer = std::async(std::launch::async, [&] {
        for (int value = 0; value < count; ++value)
        {
            queue.push(value);
        }
    });

    for (int expected = 0; expected < count; ++expected)
    {
        int actual = -1;
        queue.pop(actual);
        EXPECT_EQ(actual, expected);
    }

    producer.get();

    EXPECT_EQ(queue.size_approx(), 0u);

    int out = -1;
    EXPECT_FALSE(queue.try_pop(out));
}