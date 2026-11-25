#include "echo_client.h"
#include "frame_source.h"
#include "main_window.h"
#include "sd_discovery.h"

#include <QApplication>
#include <QObject>
#include <QString>

#include <chrono>

namespace {

constexpr std::uint16_t kSdPort = 30490;
const char* const kSdHost = "127.0.0.1";

}  // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    lab::MainWindow window;
    lab::EchoClient client;
    lab::SourceThread source;

    // 连接动作：先做服务发现，成功则用发现的地址，失败回退到界面填写值
    QObject::connect(&window, &lab::MainWindow::connect_requested, &client,
                     [&client, &window](const QString& host, quint16 port) {
                         lab::someip::sd::DiscoveredService service;
                         lab::someip::sd::SdDiscoverer discoverer(lab::EchoClient::kServiceId);

                         if (discoverer.discover(kSdHost, kSdPort,
                                                 std::chrono::milliseconds(300), service)) {
                             window.set_status(
                                 QStringLiteral("已发现服务 %1:%2，正在连接…")
                                     .arg(QString::fromStdString(service.address))
                                     .arg(service.port));
                             client.connect_to(QString::fromStdString(service.address),
                                               service.port);
                         } else {
                             window.set_status(QStringLiteral("未发现服务，使用手动地址"));
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
                     [&window, &source](const QString& message) {
                         source.stop();
                         window.set_connected(false);
                         window.set_status(QStringLiteral("连接错误: ") + message);
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
                         client.send(QString::fromUtf8(payload));
                     });

    window.resize(680, 460);
    window.show();
    return app.exec();
}