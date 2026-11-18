#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace lab {

class EpollEchoServer {
private:
    class UniqueFd {
    public:
        UniqueFd() = default;

        explicit UniqueFd(int fd) noexcept
            : fd_(fd)
        {
        }

        ~UniqueFd()
        {
            reset();
        }

        UniqueFd(const UniqueFd&) = delete;
        UniqueFd& operator=(const UniqueFd&) = delete;

        UniqueFd(UniqueFd&& other) noexcept
            : fd_(std::exchange(other.fd_, -1))
        {
        }

        UniqueFd& operator=(UniqueFd&& other) noexcept
        {
            if (this != &other) {
                reset(std::exchange(other.fd_, -1));
            }

            return *this;
        }

        int get() const noexcept
        {
            return fd_;
        }

        void reset(int fd = -1) noexcept
        {
            if (fd_ >= 0) {
                ::close(fd_);
            }

            fd_ = fd;
        }

    private:
        int fd_ = -1;
    };

    struct Connection {
        explicit Connection(int fd)
            : socket(fd)
        {
        }

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) noexcept = default;
        Connection& operator=(Connection&&) noexcept = default;

        UniqueFd socket;

        // output 中 [0, send_offset) 已经发送，剩余部分仍需续传。
        std::string output;
        std::size_t send_offset = 0;

        // 对端关闭写方向后停止读取，但剩余输出仍必须发送完。
        bool peer_closed = false;
    };

    [[noreturn]] static void throw_last_error(const char* operation)
    {
        const int error = errno;
        throw std::system_error(error, std::generic_category(), operation);
    }

public:
    explicit EpollEchoServer(std::uint16_t port)
    {
        listen_fd_.reset(::socket(
            AF_INET,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0));

        if (listen_fd_.get() < 0) {
            throw_last_error("socket");
        }

        const int reuse_address = 1;
        if (::setsockopt(
                listen_fd_.get(),
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse_address,
                sizeof(reuse_address)) < 0) {
            throw_last_error("setsockopt");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(port);
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

        if (::bind(
                listen_fd_.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) < 0) {
            throw_last_error("bind");
        }

        if (::listen(listen_fd_.get(), SOMAXCONN) < 0) {
            throw_last_error("listen");
        }

        sockaddr_in bound_address{};
        socklen_t bound_address_size = sizeof(bound_address);

        if (::getsockname(
                listen_fd_.get(),
                reinterpret_cast<sockaddr*>(&bound_address),
                &bound_address_size) < 0) {
            throw_last_error("getsockname");
        }

        port_ = ::ntohs(bound_address.sin_port);

        epoll_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
        if (epoll_fd_.get() < 0) {
            throw_last_error("epoll_create1");
        }

        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = listen_fd_.get();

        if (::epoll_ctl(
                epoll_fd_.get(),
                EPOLL_CTL_ADD,
                listen_fd_.get(),
                &event) < 0) {
            throw_last_error("epoll_ctl ADD listener");
        }
    }

    ~EpollEchoServer() = default;

    EpollEchoServer(const EpollEchoServer&) = delete;
    EpollEchoServer& operator=(const EpollEchoServer&) = delete;

    std::uint16_t port() const noexcept
    {
        return port_;
    }

    void run()
    {
        std::array<epoll_event, 64> events{};

        while (!stopped_.load(std::memory_order_acquire)) {
            // 有限超时让另一线程调用的 stop 能最终得到处理。
            const int event_count = ::epoll_wait(
                epoll_fd_.get(),
                events.data(),
                static_cast<int>(events.size()),
                50);

            if (event_count < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw_last_error("epoll_wait");
            }

            for (int index = 0; index < event_count; ++index) {
                const int fd = events[index].data.fd;
                const std::uint32_t mask = events[index].events;

                if (fd == listen_fd_.get()) {
                    accept_clients();
                    continue;
                }

                handle_client_event(fd, mask);
            }
        }
    }

    void stop() noexcept
    {
        stopped_.store(true, std::memory_order_release);
    }

private:
    static bool has_pending_output(const Connection& connection)
    {
        return connection.send_offset < connection.output.size();
    }

    void accept_clients()
    {
        for (;;) {
            const int client_fd = ::accept4(
                listen_fd_.get(),
                nullptr,
                nullptr,
                SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (client_fd >= 0) {
                add_connection(client_fd);
                continue;
            }

            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            throw_last_error("accept4");
        }
    }

    void add_connection(int client_fd)
    {
        Connection connection(client_fd);

        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = client_fd;

        if (::epoll_ctl(
                epoll_fd_.get(),
                EPOLL_CTL_ADD,
                client_fd,
                &event) < 0) {
            throw_last_error("epoll_ctl ADD client");
        }

        connections_.emplace(client_fd, std::move(connection));
    }

    void close_connection(int fd)
    {
        const auto iterator = connections_.find(fd);
        if (iterator == connections_.end()) {
            return;
        }

        // 从 epoll 注销后再销毁 fd 所有者。
        ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr);
        connections_.erase(iterator);
    }

    void update_interest(const Connection& connection)
    {
        epoll_event event{};
        event.data.fd = connection.socket.get();

        if (!connection.peer_closed) {
            event.events |= EPOLLIN;
        }

        if (has_pending_output(connection)) {
            event.events |= EPOLLOUT;
        }

        if (::epoll_ctl(
                epoll_fd_.get(),
                EPOLL_CTL_MOD,
                connection.socket.get(),
                &event) < 0) {
            throw_last_error("epoll_ctl MOD client");
        }
    }

    bool read_client(int fd)
    {
        const auto iterator = connections_.find(fd);
        if (iterator == connections_.end()) {
            return false;
        }

        Connection& connection = iterator->second;
        std::array<char, 16 * 1024> buffer{};

        const ssize_t count = ::recv(
            fd,
            buffer.data(),
            buffer.size(),
            0);

        if (count > 0) {
            connection.output.append(
                buffer.data(),
                static_cast<std::size_t>(count));

            // 读到的数据先尝试立即发送，写不完再等待 EPOLLOUT。
            return flush_client(fd);
        }

        if (count == 0) {
            connection.peer_closed = true;

            // 有剩余输出时只关闭读侧订阅，发送完后才关闭连接。
            return flush_client(fd);
        }

        if (errno == EINTR) {
            return true;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        // 单个客户端异常不影响其他连接。
        close_connection(fd);
        return false;
    }

    bool flush_client(int fd)
    {
        const auto iterator = connections_.find(fd);
        if (iterator == connections_.end()) {
            return false;
        }

        Connection& connection = iterator->second;

        while (has_pending_output(connection)) {
            const std::string_view remaining(
                connection.output.data() + connection.send_offset,
                connection.output.size() - connection.send_offset);

            const ssize_t written = ::send(
                fd,
                remaining.data(),
                remaining.size(),
                MSG_NOSIGNAL);

            if (written > 0) {
                connection.send_offset +=
                    static_cast<std::size_t>(written);
                continue;
            }

            if (written < 0 && errno == EINTR) {
                continue;
            }

            if (written < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }

            close_connection(fd);
            return false;
        }

        if (!has_pending_output(connection)) {
            connection.output.clear();
            connection.send_offset = 0;

            if (connection.peer_closed) {
                close_connection(fd);
                return false;
            }
        }

        // 只有存在剩余输出时才订阅 EPOLLOUT，避免 LT 模式持续空转。
        update_interest(connection);
        return true;
    }

    void handle_client_event(int fd, std::uint32_t events)
    {
        if (connections_.find(fd) == connections_.end()) {
            return;
        }

        if ((events & (EPOLLERR | EPOLLHUP)) != 0) {
            close_connection(fd);
            return;
        }

        if ((events & EPOLLIN) != 0) {
            if (!read_client(fd)) {
                return;
            }
        }

        if ((events & EPOLLOUT) != 0) {
            flush_client(fd);
        }
    }

private:
    UniqueFd listen_fd_;
    UniqueFd epoll_fd_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopped_{false};
    std::unordered_map<int, Connection> connections_;
};

}  // namespace lab