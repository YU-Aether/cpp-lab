#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "ring_buffer.h"

using lab::RingBuffer;

// ==================== 辅助测试类型 ====================

// 没有默认构造函数：用来验证存储载体不要求 T 可默认构造
struct Point {
    explicit Point(int v) : value(v) {}
    int value;
};

// 生命周期探针：统计存活数量、拷贝次数、移动次数和析构次数
struct Tracked {
    inline static int alive = 0;
    inline static int copies = 0;
    inline static int moves = 0;
    inline static int destroyed = 0;

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

    ~Tracked()
    {
        --alive;
        ++destroyed;
    }

    static void reset()
    {
        alive = 0;
        copies = 0;
        moves = 0;
        destroyed = 0;
    }

    int value;
};

// ==================== 基础行为 ====================

// 空缓冲区：所有查询都应该安全地返回"没有"。
TEST(RingBufferTest, EmptyOnConstruction)
{
    RingBuffer<int, 4> rb;

    EXPECT_EQ(rb.size(), 0u);
    EXPECT_TRUE(rb.empty());
    EXPECT_FALSE(rb.full());
    EXPECT_EQ(rb.front(), nullptr);
    EXPECT_EQ(rb.back(), nullptr);
    EXPECT_FALSE(rb.pop().has_value());  // 空时不能返回野值
}

// 写入后 size 增长，front/back 指向首尾。
TEST(RingBufferTest, PushUpdatesSizeAndEnds)
{
    RingBuffer<int, 4> rb;

    ASSERT_TRUE(rb.push(10));
    EXPECT_EQ(rb.size(), 1u);
    ASSERT_NE(rb.front(), nullptr);
    EXPECT_EQ(*rb.front(), 10);
    EXPECT_EQ(*rb.back(), 10);  // 只有一个元素时，首尾是同一个

    ASSERT_TRUE(rb.push(20));
    EXPECT_EQ(rb.size(), 2u);
    EXPECT_EQ(*rb.front(), 10);  // front 仍指向最老的
    EXPECT_EQ(*rb.back(), 20);   // back 跟着最新写入移动
}

// 先进先出：出队顺序必须和入队顺序一致。
TEST(RingBufferTest, FifoOrder)
{
    RingBuffer<int, 4> rb;

    ASSERT_TRUE(rb.push(1));
    ASSERT_TRUE(rb.push(2));
    ASSERT_TRUE(rb.push(3));

    EXPECT_EQ(rb.pop().value(), 1);
    EXPECT_EQ(rb.pop().value(), 2);
    EXPECT_EQ(rb.pop().value(), 3);
    EXPECT_FALSE(rb.pop().has_value());
    EXPECT_TRUE(rb.empty());
}

// 满了以后 push 必须失败，而且不能覆盖已有数据。
TEST(RingBufferTest, RejectsPushWhenFull)
{
    RingBuffer<int, 2> rb;

    ASSERT_TRUE(rb.push(1));
    ASSERT_TRUE(rb.push(2));
    EXPECT_TRUE(rb.full());

    EXPECT_FALSE(rb.push(3));   // 满了就拒绝
    EXPECT_EQ(rb.size(), 2u);   // 数量不变
    EXPECT_EQ(*rb.front(), 1);  // 老数据还在
    EXPECT_EQ(*rb.back(), 2);   // 没有被 3 覆盖
}

// 环绕：读写指针越过数组末尾后，顺序仍然正确。
TEST(RingBufferTest, WrapsAround)
{
    RingBuffer<int, 3> rb;

    ASSERT_TRUE(rb.push(1));
    ASSERT_TRUE(rb.push(2));
    ASSERT_TRUE(rb.push(3));

    ASSERT_EQ(rb.pop().value(), 1);
    ASSERT_EQ(rb.pop().value(), 2);

    // 此时 head_ 已前进到 2；再写两次，写入位置会绕回数组开头
    ASSERT_TRUE(rb.push(4));
    ASSERT_TRUE(rb.push(5));

    EXPECT_EQ(rb.pop().value(), 3);
    EXPECT_EQ(rb.pop().value(), 4);
    EXPECT_EQ(rb.pop().value(), 5);
    EXPECT_TRUE(rb.empty());
}

// 非平凡类型：验证移动语义可用，且元素能正确取出。
TEST(RingBufferTest, WorksWithNonTrivialType)
{
    RingBuffer<std::string, 2> rb;

    ASSERT_TRUE(rb.push(std::string("hello")));  // 右值，走移动重载

    std::string big(100, 'x');
    ASSERT_TRUE(rb.push(std::move(big)));        // 显式移动

    auto first = rb.pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, "hello");

    auto second = rb.pop();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->size(), 100u);             // 内容完整，说明没被截断

    EXPECT_TRUE(rb.empty());
}

// 容量为 1 的边界情况：同时是"满"和"空"的临界点。
TEST(RingBufferTest, CapacityOne)
{
    RingBuffer<int, 1> rb;

    ASSERT_TRUE(rb.push(7));
    EXPECT_TRUE(rb.full());
    EXPECT_FALSE(rb.push(8));  // 依赖"head 与 tail 不相等"的实现会在这里出错

    EXPECT_EQ(rb.pop().value(), 7);
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.front(), nullptr);
}

// ==================== 存储载体与对象生命周期 ====================

// T 不必可默认构造。
TEST(RingBufferTypeTest, SupportsNonDefaultConstructibleType)
{
    RingBuffer<Point, 4> rb;

    ASSERT_TRUE(rb.push(Point{1}));
    ASSERT_TRUE(rb.push(Point{2}));
    EXPECT_EQ(rb.size(), 2u);

    ASSERT_NE(rb.front(), nullptr);
    EXPECT_EQ(rb.front()->value, 1);
    EXPECT_EQ(rb.back()->value, 2);

    auto first = rb.pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->value, 1);
}

// 出队时元素必须被真正析构；缓冲区析构时残留元素也要析构。
TEST(RingBufferLifecycleTest, DestroysOnPopAndOnBufferDestruction)
{
    Tracked::reset();
    {
        RingBuffer<Tracked, 4> rb;
        ASSERT_TRUE(rb.push(Tracked{1}));
        ASSERT_TRUE(rb.push(Tracked{2}));
        EXPECT_EQ(Tracked::alive, 2);  // 缓冲区持有 2 个

        {
            auto out = rb.pop();
            ASSERT_TRUE(out.has_value());
            EXPECT_EQ(out->value, 1);
            // 存活 = 缓冲区里 1 个 + out 里 1 个
            EXPECT_EQ(Tracked::alive, 2);
            EXPECT_GE(Tracked::destroyed, 1);  // 出队时槽位里的对象已被销毁
        }
        // out 已析构，只剩缓冲区里的 1 个
        EXPECT_EQ(Tracked::alive, 1);
    }
    // 缓冲区析构时必须把残留元素也析构掉
    EXPECT_EQ(Tracked::alive, 0);
}

// 出队必须移动而不是拷贝。
TEST(RingBufferLifecycleTest, PopMovesInsteadOfCopies)
{
    Tracked::reset();
    {
        RingBuffer<Tracked, 2> rb;
        ASSERT_TRUE(rb.push(Tracked{42}));
        auto out = rb.pop();
        ASSERT_TRUE(out.has_value());
        EXPECT_EQ(out->value, 42);

        EXPECT_EQ(Tracked::copies, 0);  // 一次拷贝都不该发生
        EXPECT_GE(Tracked::moves, 2);   // 入队一次、出队一次
    }
    EXPECT_EQ(Tracked::alive, 0);
}

// 反复读写不能泄漏，也不能重复析构。
TEST(RingBufferLifecycleTest, NoLeakUnderHeavyUse)
{
    Tracked::reset();
    {
        RingBuffer<Tracked, 3> rb;
        for (int i = 0; i < 100; ++i) {
            rb.push(Tracked{i});
            rb.push(Tracked{i + 1});
            rb.pop();
            rb.pop();
            rb.pop();
        }
        rb.push(Tracked{1});
    }
    EXPECT_EQ(Tracked::alive, 0);
    EXPECT_EQ(Tracked::destroyed, Tracked::alive + Tracked::destroyed);  // 占位断言
}

// 支持只能移动、不能拷贝的类型。
TEST(RingBufferTypeTest, SupportsMoveOnlyType)
{
    RingBuffer<std::unique_ptr<int>, 2> rb;

    ASSERT_TRUE(rb.push(std::make_unique<int>(7)));
    auto out = rb.pop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(**out, 7);
    EXPECT_TRUE(rb.empty());
}