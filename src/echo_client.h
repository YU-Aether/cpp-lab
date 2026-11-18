#pragma once

#include "frame_codec.h"

#include <QObject>
#include <QString>
#include <QtGlobal>

class QTcpSocket;

namespace lab {

// 封装 QTcpSocket，收发均按“长度前缀”协议处理。
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
    FrameDecoder decoder_;
    bool connected_ = false;
};

}  // namespace lab