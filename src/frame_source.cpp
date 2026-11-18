#include "frame_source.h"

#include <QString>
#include <QThread>
#include <QTimer>

namespace lab {

// ---------------- FrameSource ----------------

FrameSource::FrameSource(int start_index, QObject* parent)
    : QObject(parent)
    , timer_(new QTimer(this))
    , next_index_(start_index)
{
    // 定时器是 this 的子对象，moveToThread 时会跟着一起迁移，
    // 所以它的线程亲和性始终和本对象一致。
    connect(timer_, &QTimer::timeout, this, &FrameSource::emit_frame);
}

FrameSource::~FrameSource() = default;

void FrameSource::start(int interval_ms)
{
    if (running_ || interval_ms <= 0) {
        return;
    }
    running_ = true;
    timer_->start(interval_ms);
}

void FrameSource::stop()
{
    if (!running_) {
        return;
    }
    running_ = false;
    timer_->stop();
}

bool FrameSource::is_running() const
{
    return running_;
}

void FrameSource::emit_frame()
{
    // 6 位宽、十进制、左侧补零：PLANT-000001
    const QString text =
        QStringLiteral("PLANT-%1").arg(next_index_, 6, 10, QLatin1Char('0'));
    ++next_index_;
    emit frame_produced(text.toUtf8());
}

// ---------------- SourceThread ----------------

SourceThread::SourceThread(QObject* parent)
    : QObject(parent)
{
}

SourceThread::~SourceThread()
{
    stop();
}

bool SourceThread::is_running() const
{
    return thread_ != nullptr;
}

void SourceThread::start(int interval_ms)
{
    if (thread_ != nullptr || interval_ms <= 0) {
        return;
    }

    accepting_ = true;
    thread_ = new QThread;
    worker_ = new FrameSource(next_index_);
    worker_->moveToThread(thread_);

    // 工作线程产生的帧排队回到主线程再转发；
    // 停止之后才到达的排队帧直接丢弃，避免"停了还在冒数据"。
    connect(worker_, &FrameSource::frame_produced, this,
            [this](const QByteArray& payload) {
                if (!accepting_) {
                    return;
                }
                ++next_index_;  // 下一帧接着这个编号往后排
                emit frame_produced(payload);
            },
            Qt::QueuedConnection);

    // QThread::started 是在新线程里发射的，接收者是 worker_ 且线程亲和性相同，
    // 因此这里等价于直接调用，定时器就在工作线程里被启动——这正是 QTimer 的要求。
    connect(thread_, &QThread::started, worker_, [this, interval_ms] {
        worker_->start(interval_ms);
    });

    // 关键：让工作对象在它自己的线程里销毁，主线程绝不 delete 它。
    // 线程结束时 Qt 会处理掉这个 deleteLater，wait() 返回时对象已经没了。
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);

    thread_->start();
}

void SourceThread::stop()
{
    if (thread_ == nullptr) {
        return;
    }

    QThread* thread = thread_;
    thread_ = nullptr;
    worker_ = nullptr;
    accepting_ = false;  // 之后再到的排队帧一律丢弃

    // 先退出事件循环，再等线程真正结束；
    // 定时器会随着 worker 在线程结束时一起销毁，不会有残留。
    thread->quit();
    thread->wait();
    delete thread;
}

}  // namespace lab