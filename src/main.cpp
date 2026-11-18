#include "echo_client.h"
#include "frame_source.h"
#include "main_window.h"

#include <QApplication>
#include <QObject>
#include <QString>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    lab::MainWindow window;
    lab::EchoClient client;
    lab::SourceThread source;

    // 界面动作 -> 客户端
    QObject::connect(&window, &lab::MainWindow::connect_requested,
                     &client, &lab::EchoClient::connect_to);
    QObject::connect(&window, &lab::MainWindow::disconnect_requested,
                     &client, &lab::EchoClient::disconnect_from);
    QObject::connect(&window, &lab::MainWindow::send_requested,
                     &client, &lab::EchoClient::send);

    // 客户端状态 -> 界面，同时联动数据源
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

    // 收到的报文 -> 界面：显示 服务号/方法号 + 载荷
    QObject::connect(&client, &lab::EchoClient::message_received, &window,
                     [&window](quint16 service_id, quint16 method_id, const QString& text) {
                         const QString line =
                             QStringLiteral("[0x%1/0x%2] %3")
                                 .arg(service_id, 4, 16, QLatin1Char('0'))
                                 .arg(method_id, 4, 16, QLatin1Char('0'))
                                 .arg(text);
                         window.append_received(line);
                     });

    // 数据源 -> 客户端
    QObject::connect(&source, &lab::SourceThread::frame_produced, &client,
                     [&client](const QByteArray& payload) {
                         client.send(QString::fromUtf8(payload));
                     });

    window.resize(680, 460);
    window.show();
    return app.exec();
}