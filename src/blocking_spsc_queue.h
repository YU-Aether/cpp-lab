#pragma once

#include "spsc_queue.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>

// 仅允许一个生产线程和一个消费线程。
// 该封装通过互斥锁提供阻塞语义，不再属于无锁队列。
template <typename T, std::size_t N>
class BlockingSPSCQueue {
    static_assert(N > 0, "BlockingSPSCQueue 容量必须大于 0");

public:
    BlockingSPSCQueue() = default;

    BlockingSPSCQueue(const BlockingSPSCQueue&) = delete;
    BlockingSPSCQueue& operator=(const BlockingSPSCQueue&) = delete;

    bool try_push(const T& value)
    {
        return try_push_impl(value);
    }

    bool try_push(T&& value)
    {
        return try_push_impl(std::move(value));
    }

    void push(const T& value)
    {
        push_impl(value);
    }

    void push(T&& value)
    {
        push_impl(std::move(value));
    }

    template <typename Rep, typename Period>
    bool push_for(
        const T& value,
        const std::chrono::duration<Rep, Period>& timeout)
    {
        return push_for_impl(value, timeout);
    }

    template <typename Rep, typename Period>
    bool push_for(
        T&& value,
        const std::chrono::duration<Rep, Period>& timeout)
    {
        return push_for_impl(std::move(value), timeout);
    }

    bool try_pop(T& out)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!queue_.try_pop(out)) {
                return false;
            }
        }

        // 解锁后再唤醒生产者，避免生产者醒来后立即等待同一把锁。
        not_full_.notify_one();
        return true;
    }

    void pop(T& out)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        // 谓词负责处理虚假唤醒，只有队列非空时才继续执行。
        not_empty_.wait(lock, [this] {
            return queue_.size_approx() > 0;
        });

        const bool popped = queue_.try_pop(out);
        assert(popped && "等待谓词成立后，读取应当成功");
        static_cast<void>(popped);

        lock.unlock();
        not_full_.notify_one();
    }

    template <typename Rep, typename Period>
    bool pop_for(
        T& out,
        const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        // 带谓词的 wait_for 会在虚假唤醒后继续等待剩余时间。
        const bool has_value = not_empty_.wait_for(
            lock,
            timeout,
            [this] {
                return queue_.size_approx() > 0;
            });

        if (!has_value) {
            return false;
        }

        const bool popped = queue_.try_pop(out);
        assert(popped && "超时等待确认非空后，读取应当成功");
        static_cast<void>(popped);

        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    std::size_t size_approx() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size_approx();
    }

private:
    template <typename U>
    bool try_push_impl(U&& value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!queue_.try_push(std::forward<U>(value))) {
                return false;
            }
        }

        // try_push 也必须通知，否则混合使用接口时消费者可能一直休眠。
        not_empty_.notify_one();
        return true;
    }

    template <typename U>
    void push_impl(U&& value)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        // 外层所有操作都持有同一把锁，因此这里可以把 size_approx()
        // 作为精确的等待条件；直接使用底层无锁队列时不能这样依赖它。
        not_full_.wait(lock, [this] {
            return queue_.size_approx() < N;
        });

        // 只有在确认有空间后才移动 value，等待期间不消费实参。
        const bool pushed = queue_.try_push(std::forward<U>(value));
        assert(pushed && "等待谓词成立后，写入应当成功");
        static_cast<void>(pushed);

        lock.unlock();
        not_empty_.notify_one();
    }

    template <typename U, typename Rep, typename Period>
    bool push_for_impl(
        U&& value,
        const std::chrono::duration<Rep, Period>& timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        const bool has_space = not_full_.wait_for(
            lock,
            timeout,
            [this] {
                return queue_.size_approx() < N;
            });

        if (!has_space) {
            return false;
        }

        const bool pushed = queue_.try_push(std::forward<U>(value));
        assert(pushed && "超时等待确认有空间后，写入应当成功");
        static_cast<void>(pushed);

        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

private:
    lab::SpscQueue<T, N> queue_;

    // const 接口也需要加锁，因此 mutex 必须是 mutable。
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
};