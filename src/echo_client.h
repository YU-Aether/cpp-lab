#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

class QTcpSocket;

namespace lab {

// 封装 QTcpSocket，把网络事件转换成界面可直接使用的信号。
class EchoClient : public QObject {
    Q_OBJECT

public:
    explicit EchoClient(QObject* parent = nullptr);
    ~EchoClient() override;

    void connect_to(const QString& host, quint16 port);
    void disconnect_from();
    void send(const QString& payload);

    bool is_connected() const;

signals:
    void connected();
    void disconnected();
    void data_received(const QString& text);
    void error_occurred(const QString& message);

private:
    void handle_connected();
    void handle_disconnected();
    void handle_ready_read();
    void handle_error();

private:
    QTcpSocket* socket_ = nullptr;
    bool connected_ = false;
};

}  // namespace lab