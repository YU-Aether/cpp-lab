#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace lab {

namespace detail {

[[noreturn]] inline void throw_last_socket_error(const char* operation)
{
    const int error = errno;
    throw std::system_error(error, std::generic_category(), operation);
}

// 文件描述符的唯一所有者，负责在离开作用域时关闭 fd。
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

    int get() const noexcept
    {
        return fd_;
    }

    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0) {
            // Linux 上即使 close 被信号打断，也不能盲目重试关闭同一个 fd。
            ::close(fd_);
        }

        fd_ = fd;
    }

private:
    int fd_ = -1;
};

}  // namespace detail

// 阻塞式 TCP 回显服务器，每次 serve_one 只处理一个连接。
class BlockingEchoServer {
public:
    explicit BlockingEchoServer(std::uint16_t port)
    {
        listen_fd_.reset(::socket(AF_INET, SOCK_STREAM, 0));
        if (listen_fd_.get() < 0) {
            detail::throw_last_socket_error("socket");
        }

        const int reuse_address = 1;
        if (::setsockopt(
                listen_fd_.get(),
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse_address,
                sizeof(reuse_address)) < 0) {
            detail::throw_last_socket_error("setsockopt");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(port);

        // 只监听回环地址，避免测试服务暴露到其他网络。
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

        if (::bind(
                listen_fd_.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) < 0) {
            detail::throw_last_socket_error("bind");
        }

        if (::listen(listen_fd_.get(), SOMAXCONN) < 0) {
            detail::throw_last_socket_error("listen");
        }

        // 传入端口 0 时，从内核读取实际分配的端口。
        sockaddr_in bound_address{};
        socklen_t bound_address_size = sizeof(bound_address);

        if (::getsockname(
                listen_fd_.get(),
                reinterpret_cast<sockaddr*>(&bound_address),
                &bound_address_size) < 0) {
            detail::throw_last_socket_error("getsockname");
        }

        port_ = ::ntohs(bound_address.sin_port);
    }

    ~BlockingEchoServer() = default;

    BlockingEchoServer(const BlockingEchoServer&) = delete;
    BlockingEchoServer& operator=(const BlockingEchoServer&) = delete;

    std::uint16_t port() const noexcept
    {
        return port_;
    }

    void serve_one()
    {
        int client_fd = -1;

        for (;;) {
            client_fd = ::accept(listen_fd_.get(), nullptr, nullptr);

            if (client_fd >= 0) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            detail::throw_last_socket_error("accept");
        }

        // 无论正常 EOF 还是异常退出，都必须关闭当前客户端连接。
        detail::UniqueFd client(client_fd);
        echo_connection(client.get());
    }

private:
    static void send_all(int fd, std::string_view data)
    {
        while (!data.empty()) {
            const ssize_t written = ::send(
                fd,
                data.data(),
                data.size(),
                MSG_NOSIGNAL);

            if (written > 0) {
                // send 可能只写入一部分，剩余数据必须继续发送。
                data.remove_prefix(static_cast<std::size_t>(written));
                continue;
            }

            if (written < 0 && errno == EINTR) {
                continue;
            }

            if (written == 0) {
                throw std::runtime_error("send 返回 0，无法继续发送");
            }

            detail::throw_last_socket_error("send");
        }
    }

    static void echo_connection(int client_fd)
    {
        std::array<char, 16 * 1024> buffer{};

        for (;;) {
            const ssize_t count = ::recv(
                client_fd,
                buffer.data(),
                buffer.size(),
                0);

            if (count > 0) {
                // TCP 是字节流，recv 返回多少就按多少字节回显。
                send_all(
                    client_fd,
                    std::string_view(
                        buffer.data(),
                        static_cast<std::size_t>(count)));
                continue;
            }

            if (count == 0) {
                // 对端关闭写方向，正常结束当前连接。
                return;
            }

            if (errno == EINTR) {
                continue;
            }

            detail::throw_last_socket_error("recv");
        }
    }

private:
    detail::UniqueFd listen_fd_;
    std::uint16_t port_ = 0;
};

}  // namespace lab