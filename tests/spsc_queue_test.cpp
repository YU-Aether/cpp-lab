#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "spsc_queue.h"

using lab::SpscQueue;

// 生命周期探针
struct Tracked {
    inline static int alive = 0;
    inline static int copies = 0;
    inline static int moves = 0;

    explicit Tracked(int v) : value(v) { ++alive; }

    Tracked(const Tracked& other) : value(other.value)
    {
        ++alive;
        ++copies;
    }

    Tracked(Tracked&& other) noexcept : value(other.value)
    {
        ++alive;
        ++moves;
    }

    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) noexcept = default;

    ~Tracked() { --alive; }

    static void reset()
    {
        alive = 0;
        copies = 0;
        moves = 0;
    }

    int value;
};

// 空队列：出队必须失败，不能返回野值。
TEST(SpscQueueTest, EmptyQueueRejectsPop)
{
    SpscQueue<int, 4> q;
    int out = -1;

    EXPECT_FALSE(q.try_pop(out));
    EXPECT_EQ(out, -1);  // 失败时不能改动 out
    EXPECT_EQ(q.size_approx(), 0u);
}

// 先进先出。
TEST(SpscQueueTest, FifoOrder)
{
    SpscQueue<int, 4> q;
    int out = 0;

    ASSERT_TRUE(q.try_push(1));
    ASSERT_TRUE(q.try_push(2));
    ASSERT_TRUE(q.try_push(3));
    EXPECT_EQ(q.size_approx(), 3u);

    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 1);
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 2);
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 3);
    EXPECT_FALSE(q.try_pop(out));
}

// 满时写入必须失败，且不破坏已有数据。
TEST(SpscQueueTest, RejectsPushWhenFull)
{
    SpscQueue<int, 2> q;
    int out = 0;

    ASSERT_TRUE(q.try_push(1));
    ASSERT_TRUE(q.try_push(2));
    EXPECT_FALSE(q.try_push(3));  // 满

    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 1);
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 2);
}

// 下标越过容量后必须正确绕回。
TEST(SpscQueueTest, WrapsAround)
{
    SpscQueue<int, 3> q;
    int out = 0;

    ASSERT_TRUE(q.try_push(1));
    ASSERT_TRUE(q.try_push(2));
    ASSERT_TRUE(q.try_push(3));

    ASSERT_TRUE(q.try_pop(out));
    ASSERT_EQ(out, 1);
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_EQ(out, 2);

    ASSERT_TRUE(q.try_push(4));
    ASSERT_TRUE(q.try_push(5));

    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 3);
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 4);
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 5);
}

// 容量为 1 的边界。
TEST(SpscQueueTest, CapacityOne)
{
    SpscQueue<int, 1> q;
    int out = 0;

    ASSERT_TRUE(q.try_push(7));
    EXPECT_FALSE(q.try_push(8));

    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 7);
    EXPECT_FALSE(q.try_pop(out));
}

// 元素生命周期：出队时槽位必须被销毁，队列析构时残留元素也要销毁。
TEST(SpscQueueLifecycleTest, NoLeakAcrossPushPop)
{
    Tracked::reset();
    {
        SpscQueue<Tracked, 4> q;
        Tracked out{0};
        EXPECT_EQ(Tracked::alive, 1);  // out 本身

        ASSERT_TRUE(q.try_push(Tracked{1}));
        ASSERT_TRUE(q.try_push(Tracked{2}));
        EXPECT_EQ(Tracked::alive, 3);  // out + 队列里 2 个

        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out.value, 1);
        EXPECT_EQ(Tracked::alive, 2);  // 出队后槽位已销毁
    }
    EXPECT_EQ(Tracked::alive, 0);
}

// 支持只能移动的类型。
TEST(SpscQueueTypeTest, SupportsMoveOnlyType)
{
    SpscQueue<std::unique_ptr<int>, 2> q;

    ASSERT_TRUE(q.try_push(std::make_unique<int>(7)));

    std::unique_ptr<int> out;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 7);
}

// 真实并发：一个生产者写 10 万条，一个消费者全部收到且顺序正确。
TEST(SpscQueueConcurrencyTest, SingleProducerSingleConsumer)
{
    constexpr int kTotal = 100000;

    SpscQueue<int, 64> q;
    std::atomic<bool> producer_done{false};
    std::vector<int> received;
    received.reserve(kTotal);

    std::thread producer([&q, &producer_done] {
        for (int i = 0; i < kTotal; ++i) {
            while (!q.try_push(i)) {
                std::this_thread::yield();  // 队列满，让出 CPU 重试
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    int value = 0;
    long spins = 0;
    while (received.size() < kTotal) {
        if (q.try_pop(value)) {
            received.push_back(value);
            spins = 0;
        } else if (producer_done.load(std::memory_order_acquire)) {
            // 生产已结束但队列为空，说明数据丢了；加计数上限避免死循环
            if (++spins > 10000000) {
                break;
            }
            std::this_thread::yield();
        } else {
            std::this_thread::yield();
        }
    }

    producer.join();

    ASSERT_EQ(received.size(), static_cast<std::size_t>(kTotal));
    for (int i = 0; i < kTotal; ++i) {
        ASSERT_EQ(received[static_cast<std::size_t>(i)], i);  // 顺序必须严格递增
    }
}