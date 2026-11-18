#include "echo_client.h"

#include <QMetaObject>
#include <QTcpSocket>

#include <stdexcept>

namespace lab {

EchoClient::EchoClient(QObject* parent)
    : QObject(parent)
    , socket_(new QTcpSocket(this))
{
    connect(socket_, &QTcpSocket::connected, this, &EchoClient::handle_connected);
    connect(socket_, &QTcpSocket::disconnected, this, &EchoClient::handle_disconnected);
    connect(socket_, &QTcpSocket::readyRead, this, &EchoClient::handle_ready_read);
    connect(socket_, &QTcpSocket::errorOccurred, this, &EchoClient::handle_error);
}

EchoClient::~EchoClient() = default;

void EchoClient::connect_to(const QString& host, quint16 port)
{
    // 重连前清掉旧连接和残留的半包，避免上一轮的数据污染这一轮
    socket_->abort();
    connected_ = false;
    decoder_ = someip::Decoder();
    session_id_ = 0;
    socket_->connectToHost(host, port);
}

void EchoClient::disconnect_from()
{
    if (!connected_ && socket_->state() == QAbstractSocket::UnconnectedState) {
        return;
    }

    socket_->disconnectFromHost();

    // 部分平台上 disconnectFromHost() 会同步完成并同步发 disconnected()，
    // 这里做状态兜底，保证界面状态和信号一定一致
    if (socket_->state() == QAbstractSocket::UnconnectedState) {
        handle_disconnected();
    }
}

void EchoClient::send(const QString& payload)
{
    if (!connected_ || payload.isEmpty()) {
        return;
    }

    someip::Message msg;
    msg.service_id = kServiceId;
    msg.method_id = kMethodId;
    msg.client_id = kClientId;

    // Session ID 0 是保留值，回绕时跳过
    ++session_id_;
    if (session_id_ == 0) {
        session_id_ = 1;
    }
    msg.session_id = session_id_;

    msg.interface_version = 1;
    msg.message_type = someip::MessageType::Notification;
    msg.return_code = someip::ReturnCode::Ok;

    const QByteArray utf8 = payload.toUtf8();
    msg.payload.assign(utf8.constData(), static_cast<std::size_t>(utf8.size()));

    const std::string frame = someip::encode(msg);
    socket_->write(frame.data(), static_cast<qint64>(frame.size()));
}

bool EchoClient::is_connected() const
{
    return connected_;
}

void EchoClient::handle_connected()
{
    connected_ = true;
    emit connected();
}

void EchoClient::handle_disconnected()
{
    // 幂等：可能先被 disconnect_from() 兜底调用过一次
    if (!connected_) {
        return;
    }
    connected_ = false;
    // 异步投递，保证调用方一定能在返回之后建立好信号监听
    QMetaObject::invokeMethod(this, [this] { emit disconnected(); }, Qt::QueuedConnection);
}

void EchoClient::handle_ready_read()
{
    const QByteArray chunk = socket_->readAll();
    decoder_.append(chunk.constData(), static_cast<std::size_t>(chunk.size()));

    try {
        someip::Message msg;
        while (decoder_.next_message(msg)) {
            const QString text = QString::fromUtf8(msg.payload.data(),
                                                  static_cast<int>(msg.payload.size()));
            emit message_received(msg.service_id, msg.method_id, text);
            emit data_received(text);
        }
    } catch (const std::runtime_error& error) {
        // 协议错误：这条连接已经不可信，直接断开
        socket_->abort();
        connected_ = false;
        emit error_occurred(QString::fromUtf8(error.what()));
    }
}

void EchoClient::handle_error()
{
    connected_ = false;
    emit error_occurred(socket_->errorString());
}

}  // namespace lab