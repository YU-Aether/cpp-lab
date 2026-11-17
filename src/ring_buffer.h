#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace lab {

/// 固定容量的环形缓冲区（FIFO）。
///
/// 设计约定：
///   - 容量在编译期固定为 N，运行期不做动态分配
///   - 满时 push 返回 false，且不覆盖最老的元素
///   - 空时 pop 返回 std::nullopt，front()/back() 返回 nullptr
///   - 元素只在 push 时构造、pop 时析构，因此 T 不必可默认构造，也可以是只可移动类型
///
/// 内部状态只靠两个变量维护：
///   - head_  最老元素（下一个被读走）的下标
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
        //
        // 用 emplace 在槽位里直接构造元素，而不是赋值：
        // 槽位初始是"空"状态，不存在可赋值的对象，赋值会要求 T 可默认构造。
        data_[(head_ + size_) % N].emplace(value);

        // 顺序不能反：必须先写数据再增加 size_，否则位置会算错一格。
        ++size_;
        return true;
    }

    /// 写入一个元素（移动）。语义同上。
    ///
    /// 传入右值时直接转移资源，避免 std::string / std::vector 这类类型的一次深拷贝。
    bool push(T&& value)
    {
        if (full()) {
            return false;
        }
        data_[(head_ + size_) % N].emplace(std::move(value));
        ++size_;
        return true;
    }

    /// 取出最早写入的元素；空时返回 std::nullopt。
    std::optional<T> pop()
    {
        if (empty()) {
            return std::nullopt;
        }

        const std::size_t index = head_ % N;

        // 先把元素移动出来。data_[index] 是左值表达式，不加 move 会发生一次拷贝，
        // 对持有堆内存的类型就是多余的一次分配。
        std::optional<T> value = std::move(data_[index]);

        // 显式销毁槽位里的对象。
        // 槽位不 reset 的话，元素会一直存活到整个缓冲区析构，等于泄漏。
        data_[index].reset();

        // 出队只移动读指针，不做数据搬移，因此复杂度是 O(1)。
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
        // data_[head_] 是 std::optional<T>&，先取出里面的 T，再取地址
        return &*data_[head_];
    }

    /// 最新写入的元素；空时返回 nullptr。
    const T* back() const
    {
        // size_ 是无符号类型。若不加空判断，size_ == 0 时 size_ - 1 会回绕成极大值，
        // 取模后凑巧还是个"合法"下标，但语义完全错误。
        // 所以这里的空判断是正确性必需，不是防御性冗余。
        if (empty()) {
            return nullptr;
        }
        return &*data_[(head_ + size_ - 1) % N];
    }

    std::size_t size() const { return size_; }

    static constexpr std::size_t capacity() { return N; }

    bool empty() const { return size_ == 0; }

    bool full() const { return size_ == N; }

private:
    // 每个槽位有"空 / 有值"两种状态，因此不要求 T 可默认构造，
    // 元素也可以在任意时刻构造和销毁。
    std::array<std::optional<T>, N> data_{};
    std::size_t head_ = 0;  ///< 最老元素的下标（下一个被读走的位置）
    std::size_t size_ = 0;  ///< 当前元素个数
};

}  // namespace lab