#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lab {

/// 客户端请求跟踪器：超时重发 + 重复响应去重。
///
/// 刻意不依赖 Qt、不依赖 socket，时间点全部由调用方传入，
/// 这样测试可以用假时间推进，不涉及任何真实等待。
class RequestTracker {
public:
    struct TickResult {
        std::vector<std::uint16_t> retry_sessions;
        std::vector<std::uint16_t> timed_out_sessions;
    };

    explicit RequestTracker(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
        int max_retries = 2)
        : timeout_(timeout)
        , max_retries_(max_retries)
    {
    }

    void set_timeout(std::chrono::milliseconds timeout) { timeout_ = timeout; }
    void set_max_retries(int max_retries) { max_retries_ = max_retries; }

    /// 记录一次刚发出的请求。
    void track(std::uint16_t session_id, std::string payload,
               std::chrono::steady_clock::time_point now)
    {
        Entry entry;
        entry.payload = std::move(payload);
        entry.deadline = now + timeout_;
        entry.attempts = 1;
        entries_[session_id] = std::move(entry);
    }

    /// 推进时间：返回哪些请求该重发、哪些已彻底超时。
    TickResult tick(std::chrono::steady_clock::time_point now)
    {
        TickResult result;

        for (auto& item : entries_) {
            if (now < item.second.deadline) {
                continue;
            }
            if (item.second.attempts > max_retries_) {
                result.timed_out_sessions.push_back(item.first);
                continue;
            }
            ++item.second.attempts;
            item.second.deadline = now + timeout_;
            result.retry_sessions.push_back(item.first);
        }

        // 遍历结束后再删除，避免迭代器失效
        for (const std::uint16_t session_id : result.timed_out_sessions) {
            entries_.erase(session_id);
        }
        return result;
    }

    /// 收到响应：匹配则移除并返回 true；未知 session 返回 false。
    bool acknowledge(std::uint16_t session_id)
    {
        return entries_.erase(session_id) > 0;
    }

    void clear() { entries_.clear(); }

    std::size_t size() const { return entries_.size(); }

    bool contains(std::uint16_t session_id) const
    {
        return entries_.find(session_id) != entries_.end();
    }

    /// 取回载荷用于重发；调用方必须保证该 session 存在。
    const std::string& payload_of(std::uint16_t session_id) const
    {
        return entries_.at(session_id).payload;
    }

private:
    struct Entry {
        std::string payload;
        std::chrono::steady_clock::time_point deadline;
        int attempts = 1;
    };

    std::unordered_map<std::uint16_t, Entry> entries_;
    std::chrono::milliseconds timeout_;
    int max_retries_;
};

}  // namespace lab