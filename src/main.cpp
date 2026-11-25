#include "echo_client.h"
#include "frame_source.h"
#include "main_window.h"
#include "sd_discovery.h"

#include <QApplication>
#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace {

constexpr std::uint16_t kSdPort = 30490;
const char* const kSdHost = "127.0.0.1";
const char* const kSdGroup = "224.244.224.245";
constexpr std::uint16_t kServiceId = 0x1234;
constexpr std::uint16_t kInstanceId = 0xFFFF;
constexpr std::uint16_t kEventgroupId = 0x0010;

}  // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    lab::MainWindow window;
    lab::EchoClient client;
    lab::SourceThread source;

    std::unique_ptr<lab::someip::sd::SdSubscriber> subscriber;

    QTimer event_timer;
    event_timer.setInterval(100);

    QObject::connect(&event_timer, &QTimer::timeout, [&window, &subscriber] {
        if (!subscriber) {
            return;
        }
        for (int i = 0; i < 8; ++i) {
            std::string payload;
            if (!subscriber->receive_event(payload, std::chrono::milliseconds(2))) {
                break;
            }
            window.append_received(
                QStringLiteral("[事件] %1").arg(QString::fromStdString(payload)));
        }
    });

    auto start_subscription = [&] {
        subscriber.reset();
        try {
            subscriber = std::make_unique<lab::someip::sd::SdSubscriber>(
                kServiceId, kInstanceId, kEventgroupId);

            const QString host = client.peer_host().isEmpty()
                                     ? QString::fromLatin1(kSdHost)
                                     : client.peer_host();

            if (subscriber->subscribe(host.toStdString(), kSdPort,
                                      std::chrono::milliseconds(300))) {
                window.append_received(QStringLiteral("[事件] 已订阅事件组 0x0010"));
                event_timer.start();
            } else {
                window.append_received(QStringLiteral("[事件] 订阅失败"));
                subscriber.reset();
            }
        } catch (const std::exception& error) {
            window.append_received(QStringLiteral("[事件] 订阅异常: %1")
                                       .arg(QString::fromUtf8(error.what())));
            subscriber.reset();
        }
    };

    QObject::connect(&window, &lab::MainWindow::connect_requested, &client,
                     [&client, &window](const QString& host, quint16 port) {
                         lab::someip::sd::DiscoveredService service;
                         lab::someip::sd::SdDiscoverer discoverer(lab::EchoClient::kServiceId);

                         const bool found =
                             discoverer.discover(kSdGroup, kSdPort,
                                                 std::chrono::milliseconds(300), service) ||
                             discoverer.discover(kSdHost, kSdPort,
                                                 std::chrono::milliseconds(300), service);

                         if (found) {
                             const QString address = QString::fromStdString(service.address);
                             window.set_target(address, service.port);
                             window.set_status(QStringLiteral("已发现服务 %1:%2，正在连接…")
                                                   .arg(address)
                                                   .arg(service.port));
                             client.connect_to(address, service.port);
                         } else {
                             window.set_status(QStringLiteral("未发现服务，按手动地址 %1:%2 连接…")
                                                   .arg(host)
                                                   .arg(port));
                             client.connect_to(host, port);
                         }
                     });

    QObject::connect(&window, &lab::MainWindow::disconnect_requested,
                     &client, &lab::EchoClient::disconnect_from);
    QObject::connect(&window, &lab::MainWindow::send_requested,
                     &client, &lab::EchoClient::send);

    QObject::connect(&client, &lab::EchoClient::connected, &window,
                     [&window, &source, &start_subscription] {
                         window.set_connected(true);
                         window.set_status(QStringLiteral("已连接"));
                         source.start(500);
                         start_subscription();
                     });
    QObject::connect(&client, &lab::EchoClient::disconnected, &window,
                     [&window, &source, &subscriber, &event_timer] {
                         event_timer.stop();
                         subscriber.reset();
                         source.stop();
                         window.set_connected(false);
                         window.set_status(QStringLiteral("未连接"));
                     });
    QObject::connect(&client, &lab::EchoClient::error_occurred, &window,
                     [&window, &source, &subscriber, &event_timer, &client](
                         const QString& message) {
                         event_timer.stop();
                         subscriber.reset();
                         source.stop();
                         window.set_connected(false);
                         window.set_status(QStringLiteral("连接 %1:%2 失败: %3")
                                               .arg(client.peer_host())
                                               .arg(client.peer_port())
                                               .arg(message));
                     });
    QObject::connect(&client, &lab::EchoClient::request_timeout, &window,
                     [&window](quint16 session_id) {
                         window.append_received(
                             QStringLiteral("请求超时，会话 %1 已放弃").arg(session_id));
                     });
    QObject::connect(&client, &lab::EchoClient::e2e_failed, &window,
                     [&window](quint16 session_id) {
                         window.append_received(
                             QStringLiteral("E2E 校验失败，会话 %1 的回包已丢弃")
                                 .arg(session_id));
                     });

    QObject::connect(&client, &lab::EchoClient::message_received, &window,
                     [&window](quint16 service_id, quint16 method_id, const QString& text) {
                         const QString line =
                             QStringLiteral("[0x%1/0x%2] %3")
                                 .arg(service_id, 4, 16, QLatin1Char('0'))
                                 .arg(method_id, 4, 16, QLatin1Char('0'))
                                 .arg(text);
                         window.append_received(line);
                     });

    QObject::connect(&source, &lab::SourceThread::frame_produced, &client,
                     [&client](const QByteArray& payload) {
                         if (client.pending_request_count() >= 3) {
                             return;
                         }
                         client.send(QString::fromUtf8(payload));
                     });

    window.resize(680, 460);
    window.show();
    return app.exec();
}