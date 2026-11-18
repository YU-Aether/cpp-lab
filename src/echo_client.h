#pragma once

#include "someip_codec.h"

#include <QObject>
#include <QString>
#include <QtGlobal>

class QTcpSocket;

namespace lab {

/// 基于 SOME/IP 报文的异步 TCP 客户端。
class EchoClient : public QObject {
    Q_OBJECT

public:
    explicit EchoClient(QObject* parent = nullptr);
    ~EchoClient() override;

    void connect_to(const QString& host, quint16 port);
    void disconnect_from();
    void send(const QString& payload);
    bool is_connected() const;

    // 当前固定使用的服务/方法号，后续可改成可配置
    static constexpr quint16 kServiceId = 0x1234;
    static constexpr quint16 kMethodId = 0x0001;
    static constexpr quint16 kClientId = 0x0001;

signals:
    void connected();
    void disconnected();
    /// 只带载荷文本，保持原有语义
    void data_received(const QString& text);
    /// 带协议头信息，供界面展示
    void message_received(quint16 service_id, quint16 method_id, const QString& text);
    void error_occurred(const QString& message);

private:
    void handle_connected();
    void handle_disconnected();
    void handle_ready_read();
    void handle_error();

private:
    QTcpSocket* socket_ = nullptr;
    someip::Decoder decoder_;
    bool connected_ = false;
    quint16 session_id_ = 0;
};

}  // namespace lab