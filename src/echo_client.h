#pragma once

#include "request_tracker.h"
#include "someip_codec.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include <cstddef>

class QTcpSocket;
class QTimer;

namespace lab {

/// 基于 SOME/IP 报文的异步 TCP 客户端，带请求超时重发与重复响应去重。
class EchoClient : public QObject {
    Q_OBJECT

public:
    explicit EchoClient(QObject* parent = nullptr);
    ~EchoClient() override;

    void connect_to(const QString& host, quint16 port);
    void disconnect_from();
    void send(const QString& payload);
    bool is_connected() const;

    void set_request_timeout(int milliseconds);
    void set_max_retries(int count);

    /// 还没收到响应的请求数量，供上层做流控。
    std::size_t pending_request_count() const;
    /// 本次连接实际使用的目标，供界面显示真实地址。
    QString peer_host() const { return host_; }
    quint16 peer_port() const { return port_; }

    static constexpr quint16 kServiceId = 0x1234;
    static constexpr quint16 kMethodId = 0x0001;
    static constexpr quint16 kClientId = 0x0001;

signals:
    void connected();
    void disconnected();
    void data_received(const QString& text);
    void message_received(quint16 service_id, quint16 method_id, const QString& text);
    void request_timeout(quint16 session_id);
    void error_occurred(const QString& message);

private:
    void handle_connected();
    void handle_disconnected();
    void handle_ready_read();
    void handle_error();
    void check_timeouts();
    void transmit(quint16 session_id, const QByteArray& payload);

private:
    QTcpSocket* socket_ = nullptr;
    QTimer* retry_timer_ = nullptr;
    someip::Decoder decoder_;
    RequestTracker tracker_;
    bool connected_ = false;
    quint16 session_id_ = 0;
    QString host_;
    quint16 port_ = 0;
};

}  // namespace lab