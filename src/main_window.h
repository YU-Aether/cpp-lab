#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace lab {

class MainWindow : public QWidget {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // 界面状态由外部驱动，窗口本身不发起任何网络动作。
    void set_connected(bool connected);
    void set_status(const QString& text);
    void append_received(const QString& text);

    // 把界面同步为“实际正在连接的目标”，避免输入值与真实目标不一致。
    void set_target(const QString& host, quint16 port);

signals:
    void connect_requested(const QString& host, quint16 port);
    void disconnect_requested();
    void send_requested(const QString& payload);

private:
    void handle_connect_clicked();
    void handle_send_clicked();
    void update_connect_enabled();

private:
    QLineEdit* host_edit_ = nullptr;
    QSpinBox* port_spin_ = nullptr;
    QPushButton* connect_button_ = nullptr;
    QLineEdit* send_edit_ = nullptr;
    QPushButton* send_button_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QLabel* status_label_ = nullptr;

    bool connected_ = false;
};

}  // namespace lab