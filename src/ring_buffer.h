#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace lab {

/// 固定容量的环形缓冲区（FIFO）。
///
/// 设计约定：
///   - 容量在编译期固定为 N，运行期不做任何动态分配
///   - 满时 push 返回 false，且不覆盖最老的元素（采集场景下，丢新数据比覆盖未落库的老数据安全）
///   - 空时 pop 返回 std::nullopt，front()/back() 返回 nullptr
///
/// 内部只靠两个变量维护状态：
///   - head_  最老元素（下一个要读走）的下标
///   - size_  当前元素个数
/// 写入位置永远由 (head_ + size_) % N 算出，因此不需要单独的 tail 指针。
template <typename T, std::size_t N>
class RingBuffer {
    static_assert(N > 0, "RingBuffer 容量必须大于 0");

public:
    /// 写入一个元素（拷贝）。成功返回 true；缓冲区已满返回 false。
    bool push(const T& value)
    {
        if (full()) {
            return false;
        }

        // head_ + size_ 是"最后一个元素的下一格"，也就是当前空槽位；
        // % N 让下标在到达数组末尾后绕回 0。
        data_[(head_ + size_) % N] = value;

        // 顺序不能反：必须先写数据再增加 size_，否则位置会算错一格。
        ++size_;
        return true;
    }

    /// 写入一个元素（移动）。语义同上。
    ///
    /// 这个重载的意义：传入右值（临时对象）时把内部资源直接"偷"过来，
    /// 避免 std::string / std::vector 这类类型的一次深拷贝。
    bool push(T&& value)
    {
        if (full()) {
            return false;
        }
        data_[(head_ + size_) % N] = std::move(value);
        ++size_;
        return true;
    }

    /// 取出最早写入的元素；空时返回 std::nullopt。
    std::optional<T> pop()
    {
        if (empty()) {
            return std::nullopt;
        }

        // 必须 std::move：data_[head_] 是左值表达式，不加 move 会对
        // std::string 这类类型做一次多余的深拷贝。
        T value = std::move(data_[head_]);

        // 出队只是移动"读指针"，并不擦除数据。
        // 被丢弃的槽位内容仍然存在，但已不属于缓冲区，下次 push 会自然覆盖它。
        head_ = (head_ + 1) % N;
        --size_;

        // 返回局部变量，编译器可以做 NRVO / 隐式移动，不需要再写 std::move。
        return value;
    }

    /// 最早写入的元素；空时返回 nullptr。
    const T* front() const
    {
        if (empty()) {
            return nullptr;
        }
        return &data_[head_];
    }

    /// 最新写入的元素；空时返回 nullptr。
    const T* back() const
    {
        // 注意：size_ 是 std::size_t（无符号）。若不加空判断，size_ == 0 时
        // size_ - 1 会回绕成极大值，取模后凑巧还是个"合法"下标，
        // 但语义完全错误——所以这里的空判断是正确性必需，不是防御性冗余。
        if (empty()) {
            return nullptr;
        }
        return &data_[(head_ + size_ - 1) % N];
    }

    std::size_t size() const { return size_; }

    static constexpr std::size_t capacity() { return N; }

    bool empty() const { return size_ == 0; }

    bool full() const { return size_ == N; }

private:
    std::array<T, N> data_{};  ///< 元素存储；要求 T 可默认构造
    std::size_t head_ = 0;     ///< 最老元素的下标（下一个被读走的位置）
    std::size_t size_ = 0;     ///< 当前元素个数
};

}  // namespace lab
