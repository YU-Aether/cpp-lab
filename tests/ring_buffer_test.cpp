#include <gtest/gtest.h>

#include <string>

#include "ring_buffer.h"

using lab::RingBuffer;

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

// 先进先出：pop 的顺序必须和 push 的顺序一致。
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

    EXPECT_FALSE(rb.push(3));     // 满了就拒绝
    EXPECT_EQ(rb.size(), 2u);     // 数量不变
    EXPECT_EQ(*rb.front(), 1);    // 老数据还在
    EXPECT_EQ(*rb.back(), 2);     // 没有被 3 覆盖
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

    ASSERT_TRUE(rb.push(std::string("hello")));  // 右值 → 走移动重载

    std::string big(100, 'x');
    ASSERT_TRUE(rb.push(std::move(big)));        // 显式移动

    auto first = rb.pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, "hello");

    auto second = rb.pop();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->size(), 100u);             // 内容完整，说明没有被析构/截断

    EXPECT_TRUE(rb.empty());
}

// 容量为 1 的边界情况：同时是"满"和"空"的临界点。
TEST(RingBufferTest, CapacityOne)
{
    RingBuffer<int, 1> rb;

    ASSERT_TRUE(rb.push(7));
    EXPECT_TRUE(rb.full());
    EXPECT_FALSE(rb.push(8));   // 依赖"head 与 tail 不相等"的实现会在这里出错

    EXPECT_EQ(rb.pop().value(), 7);
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.front(), nullptr);
}
