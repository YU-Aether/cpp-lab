#include "someip_codec.h"
#include "someip_server.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace {

using lab::someip::Message;
using lab::someip::MessageType;
using lab::someip::ReturnCode;

constexpr std::uint16_t kServiceId = 0x1234;
constexpr std::uint16_t kEchoMethodId = 0x0001;
constexpr std::uint16_t kClientId = 0x0007;

/// 自动关闭 fd 的小守卫，避免断言失败时漏关连接。
struct FdGuard {
    int fd = -1;
    FdGuard() = default;
    explicit FdGuard(int value) : fd(value) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    ~FdGuard()
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

/// 把服务端跑在独立线程里，析构时保证 stop + join。
struct ServerRunner {
    lab::SomeipServer& server;
    std::thread thread;

    explicit ServerRunner(lab::SomeipServer& target)
        : server(target)
        , thread([this] { server.run(); })
    {
    }

    ~ServerRunner()
    {
        server.stop();
        thread.join();
    }
};

/// 持久的流式读取器：解码器跨多次 next() 复用，
/// 这样一次 recv 读到的多条报文不会被丢弃。
struct StreamReader {
    int fd = -1;
    lab::someip::Decoder decoder;

    bool next(Message& out, int timeout_ms)
    {
        // 缓冲区里可能已经攒着上一条之后剩下的完整报文
        if (decoder.next_message(out)) {
            return true;
        }

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        char chunk[4096];

        while (true) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0) {
                return false;
            }

            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
                return false;
            }

            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                return false;
            }

            decoder.append(chunk, static_cast<std::size_t>(n));
            if (decoder.next_message(out)) {
                return true;
            }
        }
    }
};

int connect_to(std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void send_all(int fd, const std::string& data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n =
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(n);
    }
}

Message make_request(std::uint16_t method_id, std::uint16_t session_id,
                     const std::string& payload)
{
    Message msg;
    msg.service_id = kServiceId;
    msg.method_id = method_id;
    msg.client_id = kClientId;
    msg.session_id = session_id;
    msg.interface_version = 1;
    msg.message_type = MessageType::Request;
    msg.return_code = ReturnCode::Ok;
    msg.payload = payload;
    return msg;
}

/// 在给定时间内是否有任何可读数据（用来验证"服务端保持沉默"）。
bool has_data(int fd, int timeout_ms)
{
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, timeout_ms) > 0;
}

/// 等待对端关闭连接；期间收到的数据一律丢弃。
bool wait_until_closed(int fd, int timeout_ms)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char chunk[256];

    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            return false;
        }

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
            return false;
        }

        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) {
            return true;
        }
        if (n < 0) {
            return false;
        }
    }
}

}  // namespace

TEST(SomeipServerTest, RespondsToRequestEchoingPayload)
{
    lab::SomeipServer server(0);
    ServerRunner runner(server);
    FdGuard client{connect_to(server.port())};
    ASSERT_GE(client.fd, 0);

    send_all(client.fd,
             lab::someip::encode(make_request(kEchoMethodId, 42, "hello")));

    StreamReader reader{client.fd};
    Message reply;
    ASSERT_TRUE(reader.next(reply, 2000));
    EXPECT_EQ(reply.message_type, MessageType::Response);
    EXPECT_EQ(reply.return_code, ReturnCode::Ok);
    EXPECT_EQ(reply.service_id, kServiceId);
    EXPECT_EQ(reply.method_id, kEchoMethodId);
    EXPECT_EQ(reply.client_id, kClientId);
    EXPECT_EQ(reply.session_id, 42);
    EXPECT_EQ(reply.payload, "hello");
}

TEST(SomeipServerTest, RespondsToMultipleRequestsOnSameConnection)
{
    lab::SomeipServer server(0);
    ServerRunner runner(server);
    FdGuard client{connect_to(server.port())};
    ASSERT_GE(client.fd, 0);

    StreamReader reader{client.fd};
    const std::string payloads[] = {"one", "two", "three"};
    std::uint16_t session = 0;

    for (const std::string& payload : payloads) {
        ++session;
        send_all(client.fd,
                 lab::someip::encode(make_request(kEchoMethodId, session, payload)));

        Message reply;
        ASSERT_TRUE(reader.next(reply, 2000));
        EXPECT_EQ(reply.session_id, session);
        EXPECT_EQ(reply.payload, payload);
    }
}

TEST(SomeipServerTest, StaysSilentOnNotification)
{
    lab::SomeipServer server(0);
    ServerRunner runner(server);
    FdGuard client{connect_to(server.port())};
    ASSERT_GE(client.fd, 0);

    Message msg = make_request(kEchoMethodId, 1, "notify");
    msg.message_type = MessageType::Notification;
    send_all(client.fd, lab::someip::encode(msg));

    EXPECT_FALSE(has_data(client.fd, 200));

    // 连接仍然可用：再发一个 Request 依然能得到响应
    send_all(client.fd,
             lab::someip::encode(make_request(kEchoMethodId, 2, "still-alive")));
    StreamReader reader{client.fd};
    Message reply;
    ASSERT_TRUE(reader.next(reply, 2000));
    EXPECT_EQ(reply.payload, "still-alive");
}

TEST(SomeipServerTest, StaysSilentOnRequestNoReturn)
{
    lab::SomeipServer server(0);
    ServerRunner runner(server);
    FdGuard client{connect_to(server.port())};
    ASSERT_GE(client.fd, 0);

    Message msg = make_request(kEchoMethodId, 1, "fire-and-forget");
    msg.message_type = MessageType::RequestNoReturn;
    send_all(client.fd, lab::someip::encode(msg));

    EXPECT_FALSE(has_data(client.fd, 200));
}

TEST(SomeipServerTest, ReturnsUnknownMethodForUnsupportedMethodId)
{
    lab::SomeipServer server(0);
    ServerRunner runner(server);
    FdGuard client{connect_to(server.port())};
    ASSERT_GE(client.fd, 0);

    send_all(client.fd,
             lab::someip::encode(make_request(0x0099, 5, "nope")));

    StreamReader reader{client.fd};
    Message reply;
    ASSERT_TRUE(reader.next(reply, 2000));
    EXPECT_EQ(reply.message_type, MessageType::Error);
    EXPECT_EQ(reply.return_code, ReturnCode::UnknownMethod);
    // 头部字段仍需原样带回，客户端才能对上请求
    EXPECT_EQ(reply.service_id, kServiceId);
    EXPECT_EQ(reply.method_id, 0x0099);
    EXPECT_EQ(reply.client_id, kClientId);
    EXPECT_EQ(reply.session_id, 5);
}

TEST(SomeipServerTest, ReturnsWrongProtocolVersionAndCloses)
{
    lab::SomeipServer server(0);
    ServerRunner runner(server);
    FdGuard client{connect_to(server.port())};
    ASSERT_GE(client.fd, 0);

    std::string bytes =
        lab::someip::encode(make_request(kEchoMethodId, 9, "bad-version"));
    bytes[12] = static_cast<char>(0x02);  // 把协议版本改成 2
    send_all(client.fd, bytes);

    StreamReader reader{client.fd};
    Message reply;
    ASSERT_TRUE(reader.next(reply, 2000));
    EXPECT_EQ(reply.message_type, MessageType::Error);
    EXPECT_EQ(reply.return_code, ReturnCode::WrongProtocolVersion);
    EXPECT_EQ(reply.service_id, kServiceId);
    EXPECT_EQ(reply.method_id, kEchoMethodId);
    EXPECT_EQ(reply.session_id, 9);

    // 协议不可信，服务端应主动断开
    EXPECT_TRUE(wait_until_closed(client.fd, 2000));
}

TEST(SomeipServerTest, HandlesBackToBackMessagesInOneWrite)
{
    lab::SomeipServer server(0);
    ServerRunner runner(server);
    FdGuard client{connect_to(server.port())};
    ASSERT_GE(client.fd, 0);

    const std::string stream =
        lab::someip::encode(make_request(kEchoMethodId, 1, "A")) +
        lab::someip::encode(make_request(kEchoMethodId, 2, "B"));
    send_all(client.fd, stream);

    StreamReader reader{client.fd};

    Message first;
    ASSERT_TRUE(reader.next(first, 2000));
    EXPECT_EQ(first.session_id, 1);
    EXPECT_EQ(first.payload, "A");

    Message second;
    ASSERT_TRUE(reader.next(second, 2000));
    EXPECT_EQ(second.session_id, 2);
    EXPECT_EQ(second.payload, "B");
}

TEST(SomeipServerTest, ServesConcurrentClients)
{
    lab::SomeipServer server(0);
    ServerRunner runner(server);

    FdGuard first{connect_to(server.port())};
    FdGuard second{connect_to(server.port())};
    ASSERT_GE(first.fd, 0);
    ASSERT_GE(second.fd, 0);

    send_all(first.fd,
             lab::someip::encode(make_request(kEchoMethodId, 11, "from-first")));
    send_all(second.fd,
             lab::someip::encode(make_request(kEchoMethodId, 22, "from-second")));

    StreamReader reader_a{first.fd};
    StreamReader reader_b{second.fd};

    Message first_reply;
    ASSERT_TRUE(reader_a.next(first_reply, 2000));
    EXPECT_EQ(first_reply.session_id, 11);
    EXPECT_EQ(first_reply.payload, "from-first");

    Message second_reply;
    ASSERT_TRUE(reader_b.next(second_reply, 2000));
    EXPECT_EQ(second_reply.session_id, 22);
    EXPECT_EQ(second_reply.payload, "from-second");
}

TEST(SomeipServerTest, ClosesConnectionOnOversizedPayload)
{
    lab::SomeipServer server(0);
    ServerRunner runner(server);
    FdGuard client{connect_to(server.port())};
    ASSERT_GE(client.fd, 0);

    // 手工构造头部：声明载荷 2MB，超过默认 1MB 上限
    std::string raw(16, '\0');
    raw[0] = 0x12;  // Service ID 高位
    raw[1] = 0x34;
    raw[2] = 0x00;  // Method ID
    raw[3] = 0x01;
    raw[4] = 0x00;  // Length = 0x00200008（2MB + 8）
    raw[5] = 0x20;
    raw[6] = 0x00;
    raw[7] = 0x08;
    raw[12] = 0x01;  // 协议版本正确
    raw[14] = 0x00;  // Message Type = Request
    send_all(client.fd, raw);

    EXPECT_TRUE(wait_until_closed(client.fd, 2000));
}