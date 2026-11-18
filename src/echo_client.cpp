#include "echo_client.h"

#include <QTcpSocket>

#include <stdexcept>

namespace lab {

EchoClient::EchoClient(QObject* parent)
    : QObject(parent)
    , socket_(new QTcpSocket(this))
{
    // 全部使用 Qt 的异步信号，不阻塞界面线程。
    connect(socket_, &QTcpSocket::connected, this, &EchoClient::handle_connected);
    connect(socket_, &QTcpSocket::disconnected, this, &EchoClient::handle_disconnected);
    connect(socket_, &QTcpSocket::readyRead, this, &EchoClient::handle_ready_read);
    connect(socket_, &QTcpSocket::errorOccurred, this, &EchoClient::handle_error);
}

EchoClient::~EchoClient() = default;

void EchoClient::connect_to(const QString& host, quint16 port)
{
    // 清理可能残留的旧连接，避免重复点击连接导致状态混乱。
    socket_->abort();
    connected_ = false;

    // 新连接必须从干净缓冲开始，否则上一连接的半包会污染新数据。
    decoder_ = FrameDecoder();

    socket_->connectToHost(host, port);
}

void EchoClient::disconnect_from()
{
    if (!connected_) {
        return;
    }

    socket_->disconnectFromHost();

    // 部分平台上 Qt 会同步关闭连接，这里用状态兜底，保证一定会进入断开处理。
    if (socket_->state() == QAbstractSocket::UnconnectedState) {
        handle_disconnected();
    }
}

void EchoClient::send(const QString& payload)
{
    // 未连接或空内容直接丢弃，不产生错误。
    if (!connected_ || payload.isEmpty()) {
        return;
    }

    // 发送前统一加上长度前缀，服务端按同样规则解析。
    const QByteArray bytes = payload.toUtf8();
    const std::string frame = encode_frame(
        std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())));

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
    if (!connected_) {
        return;
    }

    connected_ = false;

    // 异步发出信号，保证调用方在主动断开后一定能收到通知。
    QMetaObject::invokeMethod(
        this,
        [this] { emit disconnected(); },
        Qt::QueuedConnection);
}

void EchoClient::handle_ready_read()
{
    const QByteArray data = socket_->readAll();
    if (data.isEmpty()) {
        return;
    }

    // TCP 是字节流，收到的数据先进入解码器，再按完整帧逐条上报。
    decoder_.append(data.constData(), static_cast<std::size_t>(data.size()));

    std::string frame;
    try {
        while (decoder_.next_frame(frame)) {
            emit data_received(
                QString::fromUtf8(frame.data(), static_cast<int>(frame.size())));
        }
    } catch (const std::runtime_error& error) {
        // 帧长度异常无法恢复，直接断开连接并上报原因。
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