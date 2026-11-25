#include "echo_client.h"

#include <QMetaObject>
#include <QTcpSocket>
#include <QTimer>

#include <chrono>
#include <stdexcept>
#include <string>

namespace lab {

EchoClient::EchoClient(QObject* parent)
    : QObject(parent)
    , socket_(new QTcpSocket(this))
    , retry_timer_(new QTimer(this))
{
    connect(socket_, &QTcpSocket::connected, this, &EchoClient::handle_connected);
    connect(socket_, &QTcpSocket::disconnected, this, &EchoClient::handle_disconnected);
    connect(socket_, &QTcpSocket::readyRead, this, &EchoClient::handle_ready_read);
    connect(socket_, &QTcpSocket::errorOccurred, this, &EchoClient::handle_error);

    connect(retry_timer_, &QTimer::timeout, this, &EchoClient::check_timeouts);
    retry_timer_->start(100);
}

EchoClient::~EchoClient() = default;

void EchoClient::set_request_timeout(int milliseconds)
{
    tracker_.set_timeout(std::chrono::milliseconds(milliseconds));
}

void EchoClient::set_max_retries(int count)
{
    tracker_.set_max_retries(count);
}

std::size_t EchoClient::pending_request_count() const
{
    return tracker_.size();
}

void EchoClient::connect_to(const QString& host, quint16 port)
{
    host_ = host;
    port_ = port;

    socket_->abort();
    connected_ = false;
    decoder_ = someip::Decoder();
    tracker_.clear();
    session_id_ = 0;
    socket_->connectToHost(host, port);
}

void EchoClient::disconnect_from()
{
    if (!connected_ && socket_->state() == QAbstractSocket::UnconnectedState) {
        return;
    }

    socket_->disconnectFromHost();

    if (socket_->state() == QAbstractSocket::UnconnectedState) {
        handle_disconnected();
    }
}

void EchoClient::send(const QString& payload)
{
    if (!connected_ || payload.isEmpty()) {
        return;
    }

    ++session_id_;
    if (session_id_ == 0) {
        session_id_ = 1;
    }

    const QByteArray utf8 = payload.toUtf8();
    transmit(session_id_, utf8);
    tracker_.track(session_id_,
                   std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())),
                   std::chrono::steady_clock::now());
}

bool EchoClient::is_connected() const
{
    return connected_;
}

void EchoClient::transmit(quint16 session_id, const QByteArray& payload)
{
    // 发送前加 E2E 头；计数器复用 session_id，重发时保持不变
    e2e::Frame frame;
    frame.counter = session_id;
    frame.data_id = kE2eDataId;
    frame.data.assign(payload.constData(), static_cast<std::size_t>(payload.size()));

    someip::Message msg;
    msg.service_id = kServiceId;
    msg.method_id = kMethodId;
    msg.client_id = kClientId;
    msg.session_id = session_id;
    msg.interface_version = 1;
    msg.message_type = someip::MessageType::Request;
    msg.return_code = someip::ReturnCode::Ok;
    msg.payload = e2e::protect(frame);

    const std::string bytes = someip::encode(msg);
    socket_->write(bytes.data(), static_cast<qint64>(bytes.size()));
}

void EchoClient::check_timeouts()
{
    if (!connected_) {
        return;
    }

    const RequestTracker::TickResult result =
        tracker_.tick(std::chrono::steady_clock::now());

    for (const std::uint16_t session : result.retry_sessions) {
        const std::string& payload = tracker_.payload_of(session);
        transmit(session, QByteArray(payload.data(), static_cast<int>(payload.size())));
    }

    for (const std::uint16_t session : result.timed_out_sessions) {
        emit request_timeout(session);
    }
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
    tracker_.clear();
    QMetaObject::invokeMethod(this, [this] { emit disconnected(); }, Qt::QueuedConnection);
}

void EchoClient::handle_ready_read()
{
    const QByteArray chunk = socket_->readAll();
    decoder_.append(chunk.constData(), static_cast<std::size_t>(chunk.size()));

    try {
        someip::Message msg;
        while (decoder_.next_message(msg)) {
            const bool is_reply = msg.message_type == someip::MessageType::Response ||
                                  msg.message_type == someip::MessageType::Error;

            // 匹配到在途请求就消耗掉；响应但匹配不上的，是重发导致的重复回包
            if (!tracker_.acknowledge(msg.session_id) && is_reply) {
                continue;
            }

            // 接收侧校验：CRC 对不上说明数据在传输中被破坏
            e2e::Frame frame;
            if (!e2e::unprotect(msg.payload, frame)) {
                emit e2e_failed(msg.session_id);
                continue;
            }

            const QString text = QString::fromStdString(frame.data);
            emit message_received(msg.service_id, msg.method_id, text);
            emit data_received(text);
        }
    } catch (const std::runtime_error& error) {
        socket_->abort();
        connected_ = false;
        tracker_.clear();
        emit error_occurred(QString::fromUtf8(error.what()));
    }
}

void EchoClient::handle_error()
{
    connected_ = false;
    tracker_.clear();
    emit error_occurred(socket_->errorString());
}

}  // namespace lab