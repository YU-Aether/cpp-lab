#include "epoll_echo_server.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

[[noreturn]] void throw_last_error(const char* operation)
{
    const int error = errno;
    throw std::system_error(error, std::generic_category(), operation);
}

class ClientSocket {
public:
    explicit ClientSocket(std::uint16_t port)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw_last_error("socket");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(port);

        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("inet_pton 转换回环地址失败");
        }

        if (::connect(
                fd_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) < 0) {
            const int error = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(
                error,
                std::generic_category(),
                "connect");
        }

        const timeval timeout{5, 0};

        if (::setsockopt(
                fd_,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &timeout,
                sizeof(timeout)) < 0) {
            const int error = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(
                error,
                std::generic_category(),
                "setsockopt SO_RCVTIMEO");
        }

        if (::setsockopt(
                fd_,
                SOL_SOCKET,
                SO_SNDTIMEO,
                &timeout,
                sizeof(timeout)) < 0) {
            const int error = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(
                error,
                std::generic_category(),
                "setsockopt SO_SNDTIMEO");
        }
    }

    ~ClientSocket()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    ClientSocket(const ClientSocket&) = delete;
    ClientSocket& operator=(const ClientSocket&) = delete;

    int fd() const
    {
        return fd_;
    }

    void shutdown_write()
    {
        for (;;) {
            if (::shutdown(fd_, SHUT_WR) == 0) {
                return;
            }

            if (errno == EINTR) {
                continue;
            }

            throw_last_error("shutdown");
        }
    }

private:
    int fd_ = -1;
};

// 确保测试提前失败时也会请求服务器停止并等待事件循环退出。
class ServerRunner {
public:
    explicit ServerRunner(lab::EpollEchoServer& server)
        : server_(server)
        , task_(std::async(std::launch::async, [this] {
            server_.run();
        }))
    {
    }

    ~ServerRunner()
    {
        server_.stop();

        if (task_.valid()) {
            task_.wait();
        }
    }

    ServerRunner(const ServerRunner&) = delete;
    ServerRunner& operator=(const ServerRunner&) = delete;

    void stop()
    {
        if (!task_.valid()) {
            return;
        }

        server_.stop();

        if (task_.wait_for(5s) != std::future_status::ready) {
            throw std::runtime_error("EpollEchoServer::run 未能及时返回");
        }

        // 重新抛出事件循环中的异常，让测试明确失败。
        task_.get();
    }

private:
    lab::EpollEchoServer& server_;
    std::future<void> task_;
};

void write_all(int fd, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t written = ::send(
            fd,
            data.data(),
            data.size(),
            MSG_NOSIGNAL);

        if (written > 0) {
            data.remove_prefix(static_cast<std::size_t>(written));
            continue;
        }

        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written == 0) {
            throw std::runtime_error("send 返回 0，无法继续发送");
        }

        throw_last_error("send");
    }
}

std::string read_exact(int fd, std::size_t size)
{
    std::string result(size, '\0');
    std::size_t offset = 0;

    while (offset < size) {
        const ssize_t count = ::recv(
            fd,
            result.data() + offset,
            size - offset,
            0);

        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }

        if (count == 0) {
            throw std::runtime_error("连接在读取足够数据前关闭");
        }

        if (errno == EINTR) {
            continue;
        }

        throw_last_error("recv");
    }

    return result;
}

bool reaches_eof(int fd)
{
    char buffer[64];

    for (;;) {
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);

        if (count > 0) {
            return false;
        }

        if (count == 0) {
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        throw_last_error("recv");
    }
}

std::string make_payload(std::size_t size, std::size_t seed)
{
    std::string payload(size, '\0');

    for (std::size_t index = 0; index < size; ++index) {
        payload[index] = static_cast<char>((index + seed) % 251);
    }

    return payload;
}

}  // namespace

TEST(EpollEchoServerTest, EchoesSingleMessage)
{
    lab::EpollEchoServer server(0);
    ASSERT_NE(server.port(), 0);

    ClientSocket client(server.port());
    ServerRunner runner(server);

    const std::string message = "hello epoll";
    write_all(client.fd(), message);

    EXPECT_EQ(read_exact(client.fd(), message.size()), message);

    client.shutdown_write();
    EXPECT_TRUE(reaches_eof(client.fd()));

    runner.stop();
}

TEST(EpollEchoServerTest, ServesSecondClientWhileFirstStaysIdle)
{
    lab::EpollEchoServer server(0);

    ClientSocket first(server.port());
    ClientSocket second(server.port());
    ServerRunner runner(server);

    // 第一个连接保持打开但不发数据，不能阻塞第二个连接。
    const std::string second_message = "second client";
    write_all(second.fd(), second_message);
    EXPECT_EQ(
        read_exact(second.fd(), second_message.size()),
        second_message);

    // 第二个连接仍保持打开，第一个连接之后也必须得到服务。
    const std::string first_message = "first client";
    write_all(first.fd(), first_message);
    EXPECT_EQ(
        read_exact(first.fd(), first_message.size()),
        first_message);

    runner.stop();
}

TEST(EpollEchoServerTest, HandlesConcurrentClientsIndependently)
{
    constexpr std::size_t client_count = 6;

    lab::EpollEchoServer server(0);

    std::vector<std::unique_ptr<ClientSocket>> clients;
    clients.reserve(client_count);

    for (std::size_t index = 0; index < client_count; ++index) {
        clients.push_back(
            std::make_unique<ClientSocket>(server.port()));
    }

    ServerRunner runner(server);
    std::vector<std::future<void>> client_tasks;
    client_tasks.reserve(client_count);

    for (std::size_t index = 0; index < client_count; ++index) {
        client_tasks.push_back(std::async(
            std::launch::async,
            [fd = clients[index]->fd(), index] {
                const std::string payload = make_payload(
                    32 * 1024 + index * 4096,
                    index);

                // 每个客户端同时发送和接收，避免测试端自身形成回显死锁。
                auto writer = std::async(std::launch::async, [fd, &payload] {
                    write_all(fd, payload);
                });

                const std::string echoed = read_exact(fd, payload.size());
                writer.get();

                if (echoed != payload) {
                    throw std::runtime_error("不同连接之间的数据发生错乱");
                }
            }));
    }

    for (auto& task : client_tasks) {
        task.get();
    }

    runner.stop();
}

TEST(EpollEchoServerTest, FlushesLargeBinaryPayloadAfterHalfClose)
{
    lab::EpollEchoServer server(0);
    ClientSocket client(server.port());
    ServerRunner runner(server);

    // 数据量大于常见 socket 缓冲区，用于覆盖 EAGAIN 和剩余数据续传。
    const std::string payload = make_payload(1024 * 1024, 17);

    auto writer = std::async(std::launch::async, [&] {
        write_all(client.fd(), payload);
        client.shutdown_write();
    });

    // 延迟读取，让服务端更可能遇到发送缓冲区暂时写满的情况。
    std::this_thread::sleep_for(50ms);

    const std::string echoed = read_exact(client.fd(), payload.size());

    writer.get();
    EXPECT_TRUE(echoed == payload);

    // 对端半关闭后，必须先发送完剩余数据，再关闭连接。
    EXPECT_TRUE(reaches_eof(client.fd()));

    runner.stop();
}

TEST(EpollEchoServerTest, StopReturnsWithIdleClients)
{
    lab::EpollEchoServer server(0);

    ClientSocket idle_client(server.port());
    ServerRunner runner(server);

    // 没有可读事件时，stop 也必须让事件循环退出。
    runner.stop();
}
