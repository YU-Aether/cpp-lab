#include "echo_client.h"

#include "epoll_echo_server.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>

#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>

using namespace std::chrono_literals;

namespace {

// 保证测试提前失败时也能停止服务端，避免测试进程挂死。
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

    void join()
    {
        server_.stop();

        if (!task_.valid()) {
            return;
        }

        if (task_.wait_for(5s) != std::future_status::ready) {
            throw std::runtime_error("服务器事件循环未能及时退出");
        }

        // 重新抛出事件循环中的异常，让失败原因可见。
        task_.get();
    }

private:
    lab::EpollEchoServer& server_;
    std::future<void> task_;
};

// 异步操作统一等待上限，兼顾 TSan 构建下的额外开销。
constexpr int kWaitMs = 5000;

}  // namespace

TEST(EchoClientTest, ConnectEmitsConnectedSignal)
{
    lab::EpollEchoServer server(0);
    ServerRunner runner(server);

    lab::EchoClient client;
    QSignalSpy connected_spy(&client, &lab::EchoClient::connected);

    client.connect_to(QStringLiteral("127.0.0.1"), server.port());

    ASSERT_TRUE(connected_spy.wait(kWaitMs));
    EXPECT_EQ(connected_spy.count(), 1);
    EXPECT_TRUE(client.is_connected());

    runner.join();
}

TEST(EchoClientTest, ReceivesEchoedPayload)
{
    lab::EpollEchoServer server(0);
    ServerRunner runner(server);

    lab::EchoClient client;
    QSignalSpy connected_spy(&client, &lab::EchoClient::connected);

    client.connect_to(QStringLiteral("127.0.0.1"), server.port());
    ASSERT_TRUE(connected_spy.wait(kWaitMs));

    QSignalSpy data_spy(&client, &lab::EchoClient::data_received);
    client.send(QStringLiteral("hello echo"));

    ASSERT_TRUE(data_spy.wait(kWaitMs));
    EXPECT_EQ(data_spy.at(0).at(0).toString(), QStringLiteral("hello echo"));
    EXPECT_TRUE(client.is_connected());

    runner.join();
}

TEST(EchoClientTest, ConnectionRefusedEmitsError)
{
    // 借用端口 0 获取一个真实端口，随即释放，确保该端口无人监听。
    std::uint16_t closed_port = 0;
    {
        lab::EpollEchoServer probe(0);
        closed_port = probe.port();
    }

    lab::EchoClient client;
    QSignalSpy error_spy(&client, &lab::EchoClient::error_occurred);
    QSignalSpy connected_spy(&client, &lab::EchoClient::connected);

    client.connect_to(QStringLiteral("127.0.0.1"), closed_port);

    ASSERT_TRUE(error_spy.wait(kWaitMs));
    EXPECT_EQ(connected_spy.count(), 0);
    EXPECT_FALSE(client.is_connected());
}

TEST(EchoClientTest, DisconnectEmitsDisconnected)
{
    lab::EpollEchoServer server(0);
    ServerRunner runner(server);

    lab::EchoClient client;
    QSignalSpy connected_spy(&client, &lab::EchoClient::connected);

    client.connect_to(QStringLiteral("127.0.0.1"), server.port());
    ASSERT_TRUE(connected_spy.wait(kWaitMs));

    QSignalSpy disconnected_spy(&client, &lab::EchoClient::disconnected);
    client.disconnect_from();

    ASSERT_TRUE(disconnected_spy.wait(kWaitMs));
    EXPECT_FALSE(client.is_connected());

    runner.join();
}

TEST(EchoClientTest, SendWhileDisconnectedIsIgnored)
{
    lab::EchoClient client;

    QSignalSpy data_spy(&client, &lab::EchoClient::data_received);
    QSignalSpy error_spy(&client, &lab::EchoClient::error_occurred);

    // 未连接时发送必须静默丢弃，不产生数据信号也不产生错误。
    client.send(QStringLiteral("nobody listens"));

    EXPECT_FALSE(client.is_connected());
    EXPECT_EQ(data_spy.count(), 0);
    EXPECT_EQ(error_spy.count(), 0);
}

int main(int argc, char** argv)
{
    // 必须在构造 QApplication 之前设置，确保测试走无界面后端。
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}