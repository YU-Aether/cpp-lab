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

    // 扫描间隔要明显小于请求超时；默认超时 2000ms，这里 100ms 足够
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
    // 记录本次真实目标，供状态栏在成功/失败时显示
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

    // Session ID 0 是保留值，回绕时跳过
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
    someip::Message msg;
    msg.service_id = kServiceId;
    msg.method_id = kMethodId;
    msg.client_id = kClientId;
    msg.session_id = session_id;
    msg.interface_version = 1;
    msg.message_type = someip::MessageType::Request;
    msg.return_code = someip::ReturnCode::Ok;
    msg.payload.assign(payload.constData(), static_cast<std::size_t>(payload.size()));

    const std::string frame = someip::encode(msg);
    socket_->write(frame.data(), static_cast<qint64>(frame.size()));
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
            const QString text = QString::fromUtf8(msg.payload.data(),
                                                  static_cast<int>(msg.payload.size()));

            const bool is_reply = msg.message_type == someip::MessageType::Response ||
                                  msg.message_type == someip::MessageType::Error;

            // 匹配到在途请求就消耗掉；是响应但匹配不上，说明是重发导致的重复回包
            if (!tracker_.acknowledge(msg.session_id) && is_reply) {
                continue;
            }

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