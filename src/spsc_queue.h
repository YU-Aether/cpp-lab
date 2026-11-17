#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace lab {

/// 单生产者单消费者（SPSC）无锁环形队列。
///
/// 使用约束：
///   - 同一时刻只能有一个线程调用 try_push
///   - 同一时刻只能有一个线程调用 try_pop
///   - 违反上面两条会产生数据竞争，行为未定义
///
/// 实现要点：
///   - head_ 由消费者独占写，tail_ 由生产者独占写，因此各自读写自己那个下标时
///     不需要任何同步（relaxed）
///   - 读对方的下标必须用 acquire，写完之后发布必须用 release，
///     这样"数据写入"和"下标更新"之间的顺序才不会被编译器和 CPU 重排
///   - head_ / tail_ 单调递增，不取模；取模只发生在访问数组元素时
template <typename T, std::size_t N>
class SpscQueue {
    static_assert(N > 0, "SpscQueue 容量必须大于 0");

public:
    /// 尝试写入一个元素（拷贝）。成功返回 true，队列已满返回 false。
    bool try_push(const T& value)
    {
        // 自己写的下标，只有生产者会改，用 relaxed 即可
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        // 读对方的进度必须用 acquire，确保能看到消费者已经释放的槽位
        const std::size_t head = head_.load(std::memory_order_acquire);

        if (tail - head == N) {
            return false;  // 已满
        }

        // 先把数据写进槽位，再用 release 发布下标。
        // 顺序反了消费者可能看到一个"已经就绪但数据还没写"的槽位。
        buffer_[tail % N].emplace(value);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// 尝试写入一个元素（移动）。语义同上。
    bool try_push(T&& value)
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        if (tail - head == N) {
            return false;
        }

        buffer_[tail % N].emplace(std::move(value));
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// 尝试取出一个元素写入 out。成功返回 true，队列为空返回 false。
    ///
    /// 用输出参数而不是返回值，是为了避免每次出队都构造一个 optional；
    /// 调用方可以复用同一个接收变量。
    bool try_pop(T& out)
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        if (head == tail) {
            return false;  // 已空
        }

        out = std::move(*buffer_[head % N]);

        // 必须显式销毁槽位里的对象，否则元素会一直存活
        buffer_[head % N].reset();

        // 用 release 发布"这个槽位已经空出来了"，生产者才能安全复用
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// 当前元素个数（近似值）。
    ///
    /// 两个下标分开读取，读到的可能不是同一瞬间的状态，
    /// 因此只适合日志、监控这类用途，不能用于控制流程。
    std::size_t size_approx() const
    {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    static constexpr std::size_t capacity() { return N; }

private:
    std::array<std::optional<T>, N> buffer_{};

    // 两个原子变量分到不同缓存行：如果落在同一行，生产者和消费者会互相
    // 使对方的缓存行失效，性能可能下降数倍（伪共享）。
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace lab