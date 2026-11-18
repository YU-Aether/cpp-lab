#include "echo_client.h"
#include "main_window.h"

#include <QApplication>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    lab::MainWindow window;
    lab::EchoClient client;

    // 界面只发请求，网络动作全部交给 EchoClient。
    QObject::connect(
        &window,
        &lab::MainWindow::connect_requested,
        &client,
        [&client](const QString& host, quint16 port) {
            client.connect_to(host, port);
        });

    QObject::connect(
        &window,
        &lab::MainWindow::disconnect_requested,
        &client,
        &lab::EchoClient::disconnect_from);

    QObject::connect(
        &window,
        &lab::MainWindow::send_requested,
        &client,
        &lab::EchoClient::send);

    QObject::connect(&client, &lab::EchoClient::connected, &window, [&window] {
        window.set_connected(true);
        window.set_status(QStringLiteral("已连接"));
    });

    QObject::connect(&client, &lab::EchoClient::disconnected, &window, [&window] {
        window.set_connected(false);
        window.set_status(QStringLiteral("未连接"));
    });

    QObject::connect(
        &client,
        &lab::EchoClient::data_received,
        &window,
        &lab::MainWindow::append_received);

    QObject::connect(
        &client,
        &lab::EchoClient::error_occurred,
        &window,
        [&window](const QString& message) {
            window.set_connected(false);
            window.set_status(QStringLiteral("连接错误: ") + message);
        });

    window.resize(680, 460);
    window.show();

    return app.exec();
}