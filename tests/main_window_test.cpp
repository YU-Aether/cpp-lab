#include "main_window.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>

namespace {

// 测试全部运行在无界面平台，避免依赖真实显示环境。
class MainWindowTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        host_edit_ = window_.findChild<QLineEdit*>(QStringLiteral("hostEdit"));
        port_spin_ = window_.findChild<QSpinBox*>(QStringLiteral("portSpin"));
        connect_button_ = window_.findChild<QPushButton*>(QStringLiteral("connectButton"));
        send_edit_ = window_.findChild<QLineEdit*>(QStringLiteral("sendEdit"));
        send_button_ = window_.findChild<QPushButton*>(QStringLiteral("sendButton"));
        log_view_ = window_.findChild<QPlainTextEdit*>(QStringLiteral("logView"));
        status_label_ = window_.findChild<QLabel*>(QStringLiteral("statusLabel"));

        ASSERT_NE(host_edit_, nullptr) << "缺少 hostEdit 控件";
        ASSERT_NE(port_spin_, nullptr) << "缺少 portSpin 控件";
        ASSERT_NE(connect_button_, nullptr) << "缺少 connectButton 控件";
        ASSERT_NE(send_edit_, nullptr) << "缺少 sendEdit 控件";
        ASSERT_NE(send_button_, nullptr) << "缺少 sendButton 控件";
        ASSERT_NE(log_view_, nullptr) << "缺少 logView 控件";
        ASSERT_NE(status_label_, nullptr) << "缺少 statusLabel 控件";
    }

    lab::MainWindow window_;

    QLineEdit* host_edit_ = nullptr;
    QSpinBox* port_spin_ = nullptr;
    QPushButton* connect_button_ = nullptr;
    QLineEdit* send_edit_ = nullptr;
    QPushButton* send_button_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QLabel* status_label_ = nullptr;
};

}  // namespace

TEST_F(MainWindowTest, InitialStateIsDisconnected)
{
    EXPECT_EQ(host_edit_->text(), QStringLiteral("127.0.0.1"));
    EXPECT_EQ(port_spin_->value(), 9555);
    EXPECT_EQ(connect_button_->text(), QStringLiteral("连接"));
    EXPECT_TRUE(connect_button_->isEnabled());
    EXPECT_FALSE(send_button_->isEnabled());
    EXPECT_TRUE(log_view_->isReadOnly());
    EXPECT_EQ(status_label_->text(), QStringLiteral("未连接"));
}

TEST_F(MainWindowTest, EmptyHostDisablesConnectButton)
{
    QSignalSpy spy(&window_, &lab::MainWindow::connect_requested);

    host_edit_->setText(QString());
    EXPECT_FALSE(connect_button_->isEnabled());

    // 禁用状态下点击不能发出连接请求。
    connect_button_->click();
    EXPECT_EQ(spy.count(), 0);

    host_edit_->setText(QStringLiteral("127.0.0.1"));
    EXPECT_TRUE(connect_button_->isEnabled());
}

TEST_F(MainWindowTest, ConnectClickEmitsRequestWithoutChangingUi)
{
    QSignalSpy spy(&window_, &lab::MainWindow::connect_requested);

    port_spin_->setValue(9000);
    connect_button_->click();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), QStringLiteral("127.0.0.1"));
    EXPECT_EQ(spy.at(0).at(1).value<quint16>(), static_cast<quint16>(9000));

    // 点击只发出请求，界面状态必须由 set_connected 决定。
    EXPECT_EQ(connect_button_->text(), QStringLiteral("连接"));
    EXPECT_FALSE(send_button_->isEnabled());
}

TEST_F(MainWindowTest, SetConnectedUpdatesInteractiveState)
{
    window_.set_connected(true);
    EXPECT_EQ(connect_button_->text(), QStringLiteral("断开"));
    EXPECT_TRUE(send_button_->isEnabled());

    window_.set_connected(false);
    EXPECT_EQ(connect_button_->text(), QStringLiteral("连接"));
    EXPECT_FALSE(send_button_->isEnabled());
}

TEST_F(MainWindowTest, ConnectedClickEmitsDisconnectRequest)
{
    window_.set_connected(true);

    QSignalSpy connect_spy(&window_, &lab::MainWindow::connect_requested);
    QSignalSpy disconnect_spy(&window_, &lab::MainWindow::disconnect_requested);

    connect_button_->click();

    EXPECT_EQ(connect_spy.count(), 0);
    EXPECT_EQ(disconnect_spy.count(), 1);
}

TEST_F(MainWindowTest, SetStatusUpdatesLabel)
{
    window_.set_status(QStringLiteral("连接中"));
    EXPECT_EQ(status_label_->text(), QStringLiteral("连接中"));

    window_.set_status(QStringLiteral("连接失败"));
    EXPECT_EQ(status_label_->text(), QStringLiteral("连接失败"));
}

TEST_F(MainWindowTest, SendEmitsPayloadAndClearsInput)
{
    window_.set_connected(true);

    QSignalSpy spy(&window_, &lab::MainWindow::send_requested);

    send_edit_->setText(QStringLiteral("hello plant"));
    send_button_->click();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), QStringLiteral("hello plant"));
    EXPECT_TRUE(send_edit_->text().isEmpty());
}

TEST_F(MainWindowTest, EmptyPayloadDoesNotEmit)
{
    window_.set_connected(true);

    QSignalSpy spy(&window_, &lab::MainWindow::send_requested);

    send_button_->click();
    EXPECT_EQ(spy.count(), 0);

    // 只有空白字符同样视为空内容。
    send_edit_->setText(QStringLiteral("   "));
    send_button_->click();
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(MainWindowTest, AppendReceivedAddsOneLinePerCall)
{
    window_.append_received(QStringLiteral("ack 1"));
    window_.append_received(QStringLiteral("ack 2"));

    EXPECT_EQ(log_view_->toPlainText(), QStringLiteral("ack 1\nack 2\n"));
    EXPECT_TRUE(log_view_->isReadOnly());
}

int main(int argc, char** argv)
{
    // 必须在构造 QApplication 之前设置，确保所有用例走无界面后端。
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}