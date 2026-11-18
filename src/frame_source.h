#pragma once

#include <QByteArray>
#include <QObject>

class QThread;
class QTimer;

namespace lab {

/// 数据源：在“自己所属的线程”里按固定间隔产生一帧模拟数据。
///
/// 它不负责创建线程，线程亲和性由外层 SourceThread 决定。
/// 因为定时器和帧产生都在同一个线程内，所以内部完全不需要加锁。
class FrameSource : public QObject {
    Q_OBJECT

public:
    explicit FrameSource(int start_index = 1, QObject* parent = nullptr);
    ~FrameSource() override;

    /// 在当前线程启动定时器。必须在对象所属线程里调用。
    void start(int interval_ms);
    /// 停掉定时器，可重复调用。
    void stop();
    bool is_running() const;

signals:
    void frame_produced(const QByteArray& payload);

private:
    void emit_frame();

    QTimer* timer_ = nullptr;
    bool running_ = false;
    int next_index_ = 1;
};

/// 线程封装：持有工作线程与 FrameSource，把帧转发回主线程。
///
/// 使用方式（都在主线程调用）：
///   source.start(500);
///   source.stop();
/// 析构时会自动 stop()，不会留下未回收的线程。
class SourceThread : public QObject {
    Q_OBJECT

public:
    explicit SourceThread(QObject* parent = nullptr);
    ~SourceThread() override;

    void start(int interval_ms);
    void stop();
    bool is_running() const;

signals:
    void frame_produced(const QByteArray& payload);

private:
    QThread* thread_ = nullptr;
    FrameSource* worker_ = nullptr;
    bool accepting_ = false;
    int next_index_ = 1;
};

}  // namespace lab