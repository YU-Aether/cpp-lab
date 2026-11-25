#pragma once

#include "someip_codec.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace lab {

/// 本示例约定的服务号与方法号，与上位机的 EchoClient 保持一致。
inline constexpr std::uint16_t kEchoServiceId = 0x1234;
inline constexpr std::uint16_t kEchoMethodId = 0x0001;

namespace someip_detail {

/// 独占 fd，析构自动 close。
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}

    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const { return fd_; }

    void reset(int fd = -1)
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

    int release()
    {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

}  // namespace someip_detail

/// 单线程 epoll + 水平触发的 SOME/IP 服务端。
///
/// 请求处理规则：
///   - Request + 方法 0x0001 -> Response，头部原样带回，载荷回显
///   - Request + 其他方法号   -> Error + UnknownMethod
///   - RequestNoReturn / Notification -> 不响应
///   - 协议版本不符           -> Error + WrongProtocolVersion，随后关闭连接
///   - 长度非法 / 载荷超上限  -> 直接关闭连接
class SomeipServer {
public:
    explicit SomeipServer(std::uint16_t port)
        : listen_fd_(::socket(AF_INET, SOCK_STREAM, 0))
        , epoll_fd_(-1)
    {
        if (!listen_fd_) {
            throw std::runtime_error("创建监听 socket 失败");
        }

        int reuse = 1;
        if (::setsockopt(listen_fd_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                         sizeof(reuse)) != 0) {
            throw std::runtime_error("设置 SO_REUSEADDR 失败");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (::bind(listen_fd_.get(), reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0) {
            throw std::runtime_error("绑定端口失败");
        }

        if (::listen(listen_fd_.get(), 64) != 0) {
            throw std::runtime_error("监听失败");
        }

        set_non_blocking(listen_fd_.get());

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listen_fd_.get(), reinterpret_cast<sockaddr*>(&bound),
                          &len) != 0) {
            throw std::runtime_error("获取监听端口失败");
        }
        port_ = ntohs(bound.sin_port);

        epoll_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
        if (!epoll_fd_) {
            throw std::runtime_error("创建 epoll 失败");
        }

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd_.get();
        if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, listen_fd_.get(), &ev) != 0) {
            throw std::runtime_error("注册监听 fd 失败");
        }
    }

    ~SomeipServer()
    {
        // 先让 run() 退出，再释放 fd，避免事件循环访问已关闭的描述符
        stop();
        if (thread_joinable_hint_) {
            // 实际 join 由调用方负责，这里只保证 fd 不被提前复用
        }
    }

    SomeipServer(const SomeipServer&) = delete;
    SomeipServer& operator=(const SomeipServer&) = delete;

    std::uint16_t port() const { return port_; }

    /// 阻塞运行事件循环，直到 stop() 被调用。
    void run()
    {
        epoll_event events[64];

        while (!stopped_.load(std::memory_order_relaxed)) {
            const int ready = ::epoll_wait(epoll_fd_.get(), events, 64, 50);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("epoll_wait 失败");
            }

            for (int i = 0; i < ready; ++i) {
                const int fd = events[i].data.fd;
                const std::uint32_t mask = events[i].events;

                if (fd == listen_fd_.get()) {
                    accept_clients();
                    continue;
                }

                if ((mask & EPOLLERR) != 0) {
                    close_connection(fd);
                    continue;
                }

                if ((mask & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) != 0) {
                    handle_readable(fd);
                }

                if ((mask & EPOLLOUT) != 0) {
                    auto it = connections_.find(fd);
                    if (it != connections_.end()) {
                        flush_connection(fd, it->second);
                        if (should_close(it->second)) {
                            close_connection(fd);
                        } else {
                            update_interest(fd, it->second);
                        }
                    }
                }
            }
        }
    }

    /// 请求退出；最多等一个 epoll_wait 超时周期（50ms）。
    void stop() { stopped_.store(true); }

private:
    struct Connection {
        someip_detail::UniqueFd socket;
        someip::Decoder decoder;
        std::string output;
        std::size_t send_offset = 0;
        bool close_after_flush = false;
    };

    static void set_non_blocking(int fd)
    {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    void accept_clients()
    {
        while (true) {
            sockaddr_in peer{};
            socklen_t len = sizeof(peer);
            const int fd = ::accept4(listen_fd_.get(),
                                     reinterpret_cast<sockaddr*>(&peer), &len,
                                     SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                // EAGAIN：连接队列已空
                return;
            }

            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = fd;
            if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
                ::close(fd);
                continue;
            }

            Connection conn;
            conn.socket.reset(fd);
            connections_.emplace(fd, std::move(conn));
        }
    }

    void handle_readable(int fd)
    {
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return;
        }
        Connection& conn = it->second;

        bool peer_closed = false;
        char buffer[4096];

        while (true) {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                conn.decoder.append(buffer, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                peer_closed = true;
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            peer_closed = true;
            break;
        }

        try {
            someip::Message msg;
            while (conn.decoder.next_message(msg)) {
                handle_message(conn, msg);
            }
        } catch (const someip::ProtocolError& error) {
            handle_protocol_error(conn, error);
        }

        if (peer_closed) {
            // 对端已半关闭：把待发数据刷完再关
            conn.close_after_flush = true;
        }

        flush_connection(fd, conn);
        if (should_close(conn)) {
            close_connection(fd);
            return;
        }
        update_interest(fd, conn);
    }

    static void handle_message(Connection& conn, const someip::Message& request)
    {
        // 只有 Request 需要应答；Notification / RequestNoReturn 静默丢弃
        if (request.message_type != someip::MessageType::Request) {
            return;
        }

        someip::Message reply = request;  // 头部字段全部原样带回

        if (request.method_id == kEchoMethodId) {
            reply.message_type = someip::MessageType::Response;
            reply.return_code = someip::ReturnCode::Ok;
        } else {
            reply.message_type = someip::MessageType::Error;
            reply.return_code = someip::ReturnCode::UnknownMethod;
            reply.payload.clear();
        }

        conn.output += someip::encode(reply);
    }

    static void handle_protocol_error(Connection& conn,
                                      const someip::ProtocolError& error)
    {
        if (error.reason() == someip::ProtocolError::Reason::VersionMismatch) {
            someip::Message reply = error.header();
            // 回包本身必须是合法协议版本，否则对端也解不出来
            reply.protocol_version = someip::kProtocolVersion;
            reply.message_type = someip::MessageType::Error;
            reply.return_code = someip::ReturnCode::WrongProtocolVersion;
            reply.payload.clear();
            conn.output += someip::encode(reply);
        }

        conn.close_after_flush = true;
    }

    void flush_connection(int fd, Connection& conn)
    {
        while (conn.send_offset < conn.output.size()) {
            const ssize_t n =
                ::send(fd, conn.output.data() + conn.send_offset,
                       conn.output.size() - conn.send_offset, MSG_NOSIGNAL);
            if (n > 0) {
                conn.send_offset += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;  // 等下一次 EPOLLOUT
            }
            conn.close_after_flush = true;
            conn.send_offset = conn.output.size();
            return;
        }

        conn.output.clear();
        conn.send_offset = 0;
    }

    static bool should_close(const Connection& conn)
    {
        return conn.close_after_flush && conn.send_offset >= conn.output.size();
    }

    void update_interest(int fd, const Connection& conn)
    {
        // 只在确实有数据待发时才订阅 EPOLLOUT，避免空转
        std::uint32_t events = EPOLLIN;
        if (conn.send_offset < conn.output.size()) {
            events |= EPOLLOUT;
        }

        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev);
    }

    void close_connection(int fd)
    {
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return;
        }
        ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr);
        connections_.erase(it);
    }

private:
    someip_detail::UniqueFd listen_fd_;
    someip_detail::UniqueFd epoll_fd_;
    std::unordered_map<int, Connection> connections_;
    std::atomic<bool> stopped_{false};
    std::uint16_t port_ = 0;
    bool thread_joinable_hint_ = false;
};

}  // namespace lab