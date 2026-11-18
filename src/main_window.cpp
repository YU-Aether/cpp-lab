#include "main_window.h"
#include <QTextCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace lab {

MainWindow::MainWindow(QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("PlantPulse 调试上位机"));

    host_edit_ = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    host_edit_->setObjectName(QStringLiteral("hostEdit"));

    port_spin_ = new QSpinBox(this);
    port_spin_->setObjectName(QStringLiteral("portSpin"));
    port_spin_->setRange(1, 65535);
    port_spin_->setValue(9555);

    connect_button_ = new QPushButton(QStringLiteral("连接"), this);
    connect_button_->setObjectName(QStringLiteral("connectButton"));

    auto* address_layout = new QHBoxLayout();
    address_layout->addWidget(new QLabel(QStringLiteral("服务端:"), this));
    address_layout->addWidget(host_edit_, 1);
    address_layout->addWidget(new QLabel(QStringLiteral("端口:"), this));
    address_layout->addWidget(port_spin_);
    address_layout->addWidget(connect_button_);

    send_edit_ = new QLineEdit(this);
    send_edit_->setObjectName(QStringLiteral("sendEdit"));
    send_edit_->setPlaceholderText(QStringLiteral("输入要发送的数据"));
    send_edit_->setEnabled(false);

    send_button_ = new QPushButton(QStringLiteral("发送"), this);
    send_button_->setObjectName(QStringLiteral("sendButton"));
    send_button_->setEnabled(false);

    auto* send_layout = new QHBoxLayout();
    send_layout->addWidget(send_edit_, 1);
    send_layout->addWidget(send_button_);

    log_view_ = new QPlainTextEdit(this);
    log_view_->setObjectName(QStringLiteral("logView"));
    log_view_->setReadOnly(true);

    status_label_ = new QLabel(QStringLiteral("未连接"), this);
    status_label_->setObjectName(QStringLiteral("statusLabel"));

    auto* main_layout = new QVBoxLayout(this);
    main_layout->addLayout(address_layout);
    main_layout->addLayout(send_layout);
    main_layout->addWidget(log_view_, 1);
    main_layout->addWidget(status_label_);

    connect(host_edit_, &QLineEdit::textChanged, this, [this] {
        update_connect_enabled();
    });
    connect(connect_button_, &QPushButton::clicked, this, &MainWindow::handle_connect_clicked);
    connect(send_button_, &QPushButton::clicked, this, &MainWindow::handle_send_clicked);
    connect(send_edit_, &QLineEdit::returnPressed, this, &MainWindow::handle_send_clicked);

    update_connect_enabled();
}

void MainWindow::set_connected(bool connected)
{
    connected_ = connected;

    connect_button_->setText(connected ? QStringLiteral("断开") : QStringLiteral("连接"));
    send_edit_->setEnabled(connected);
    send_button_->setEnabled(connected);

    // 已连接时即使地址被清空，也必须保留断开入口。
    update_connect_enabled();
}

void MainWindow::set_status(const QString& text)
{
    status_label_->setText(text);
}

void MainWindow::append_received(const QString& text)
{
    // 逐行追加并显式补换行，保证每次调用占一行且末尾保留换行。
    log_view_->moveCursor(QTextCursor::End);
    log_view_->insertPlainText(text + QLatin1Char('\n'));
    log_view_->ensureCursorVisible();
}

void MainWindow::handle_connect_clicked()
{
    if (connected_) {
        emit disconnect_requested();
        return;
    }

    const QString host = host_edit_->text().trimmed();
    if (host.isEmpty()) {
        return;
    }

    emit connect_requested(host, static_cast<quint16>(port_spin_->value()));
}

void MainWindow::handle_send_clicked()
{
    const QString payload = send_edit_->text();

    // 未连接或只有空白字符时不发送任何内容。
    if (!connected_ || payload.trimmed().isEmpty()) {
        return;
    }

    emit send_requested(payload);
    send_edit_->clear();
}

void MainWindow::update_connect_enabled()
{
    const bool has_host = !host_edit_->text().trimmed().isEmpty();
    connect_button_->setEnabled(connected_ || has_host);
}

}  // namespace lab