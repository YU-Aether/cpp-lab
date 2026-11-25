#include "echo_client.h"
#include "frame_source.h"
#include "main_window.h"
#include "sd_discovery.h"

#include <QApplication>
#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>

namespace {

constexpr std::uint16_t kSdPort = 30490;
const char* const kSdHost = "127.0.0.1";
const char* const kSdGroup = "224.244.224.245";

}  // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    lab::MainWindow window;
    lab::EchoClient client;
    lab::SourceThread source;

    // 连接动作：先多播发现，再单播发现，都失败才用手动地址
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
                             // 先把界面同步成真实目标，再发起连接
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
                     [&window, &source] {
                         window.set_connected(true);
                         window.set_status(QStringLiteral("已连接"));
                         source.start(500);
                     });
    QObject::connect(&client, &lab::EchoClient::disconnected, &window,
                     [&window, &source] {
                         source.stop();
                         window.set_connected(false);
                         window.set_status(QStringLiteral("未连接"));
                     });
    QObject::connect(&client, &lab::EchoClient::error_occurred, &window,
                     [&window, &source, &client](const QString& message) {
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
                         // 在途请求过多就跳过这一帧，避免服务端不回时超时堆积
                         if (client.pending_request_count() >= 3) {
                             return;
                         }
                         client.send(QString::fromUtf8(payload));
                     });

    window.resize(680, 460);
    window.show();
    return app.exec();
}