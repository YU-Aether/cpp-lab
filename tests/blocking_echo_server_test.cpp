#include "blocking_echo_server.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

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

        // 防止服务端实现错误时，测试永久阻塞在 recv。
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
                "setsockopt");
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

}  // namespace

TEST(BlockingEchoServerTest, EchoesSingleMessage)
{
    lab::BlockingEchoServer server(0);
    ASSERT_NE(server.port(), 0);

    ClientSocket client(server.port());
    auto server_task = std::async(std::launch::async, [&] {
        server.serve_one();
    });

    const std::string message = "hello over tcp";
    write_all(client.fd(), message);

    EXPECT_EQ(read_exact(client.fd(), message.size()), message);

    client.shutdown_write();
    EXPECT_EQ(server_task.wait_for(5s), std::future_status::ready);
    server_task.get();
}

TEST(BlockingEchoServerTest, EchoesSeveralMessagesOnOneConnection)
{
    lab::BlockingEchoServer server(0);
    ClientSocket client(server.port());

    auto server_task = std::async(std::launch::async, [&] {
        server.serve_one();
    });

    const std::array<std::string, 3> messages{
        "first message",
        "second message",
        "third message"
    };

    for (const auto& message : messages) {
        write_all(client.fd(), message);
        EXPECT_EQ(read_exact(client.fd(), message.size()), message);
    }

    client.shutdown_write();
    EXPECT_EQ(server_task.wait_for(5s), std::future_status::ready);
    server_task.get();
}

TEST(BlockingEchoServerTest, EchoesLargeBinaryPayload)
{
    lab::BlockingEchoServer server(0);
    ClientSocket client(server.port());

    auto server_task = std::async(std::launch::async, [&] {
        server.serve_one();
    });

    // 包含 '\0' 和非 ASCII 字节，验证实现按字节数处理，而非按 C 字符串处理。
    std::string payload(256 * 1024, '\0');
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>(index % 251);
    }

    // 同时发送和接收，避免双方缓冲区填满后互相等待。
    auto writer_task = std::async(std::launch::async, [&] {
        write_all(client.fd(), payload);
        client.shutdown_write();
    });

    const std::string echoed = read_exact(client.fd(), payload.size());

    writer_task.get();
    EXPECT_TRUE(echoed == payload);

    EXPECT_EQ(server_task.wait_for(5s), std::future_status::ready);
    server_task.get();
}

TEST(BlockingEchoServerTest, ClosesConnectionAfterClientEof)
{
    lab::BlockingEchoServer server(0);
    ClientSocket client(server.port());

    auto server_task = std::async(std::launch::async, [&] {
        server.serve_one();
    });

    // 客户端不再发送数据，服务端应退出当前连接的处理流程。
    client.shutdown_write();

    // serve_one 返回后，服务端应关闭已连接 socket。
    EXPECT_TRUE(reaches_eof(client.fd()));
    EXPECT_EQ(server_task.wait_for(5s), std::future_status::ready);
    server_task.get();
}

TEST(BlockingEchoServerTest, ServesClientsSequentially)
{
    lab::BlockingEchoServer server(0);

    const auto exchange_once = [&server](const std::string& message) {
        ClientSocket client(server.port());

        auto server_task = std::async(std::launch::async, [&] {
            server.serve_one();
        });

        write_all(client.fd(), message);
        EXPECT_EQ(read_exact(client.fd(), message.size()), message);

        client.shutdown_write();
        EXPECT_EQ(server_task.wait_for(5s), std::future_status::ready);
        server_task.get();
    };

    // 一个连接结束不能关闭监听 socket，后续 serve_one 仍应正常工作。
    exchange_once("first client");
    exchange_once("second client");
}