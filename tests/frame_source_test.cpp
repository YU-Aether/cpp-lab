#include "frame_source.h"

#include <gtest/gtest.h>
#include <QTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QString>

#include <algorithm>

namespace {

// 等待信号累计到指定次数，用于容忍定时器与线程调度的抖动。
bool wait_for_count(QSignalSpy& spy, int target, int timeout_ms)
{
    QElapsedTimer timer;
    timer.start();

    while (spy.count() < target) {
        const int remaining = timeout_ms - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            return false;
        }

        spy.wait(std::min(remaining, 50));
    }

    return true;
}

QString payload_at(const QSignalSpy& spy, int index)
{
    return QString::fromUtf8(spy.at(index).at(0).toByteArray());
}

}  // namespace

TEST(FrameSourceTest, DoesNotEmitBeforeStart)
{
    lab::SourceThread source;
    QSignalSpy spy(&source, &lab::SourceThread::frame_produced);

    QTest::qWait(150);

    EXPECT_EQ(spy.count(), 0);
    EXPECT_FALSE(source.is_running());
}

TEST(FrameSourceTest, EmitsFramesAtInterval)
{
    lab::SourceThread source;
    QSignalSpy spy(&source, &lab::SourceThread::frame_produced);

    source.start(20);

    ASSERT_TRUE(wait_for_count(spy, 3, 3000));
    EXPECT_TRUE(source.is_running());

    source.stop();
}

TEST(FrameSourceTest, PayloadSequenceIsIncrementing)
{
    lab::SourceThread source;
    QSignalSpy spy(&source, &lab::SourceThread::frame_produced);

    source.start(20);
    ASSERT_TRUE(wait_for_count(spy, 2, 3000));
    source.stop();

    // 每帧载荷格式固定，序号从 1 开始补零到 6 位。
    EXPECT_EQ(payload_at(spy, 0), QStringLiteral("PLANT-000001"));
    EXPECT_EQ(payload_at(spy, 1), QStringLiteral("PLANT-000002"));
}

TEST(FrameSourceTest, StopStopsEmissions)
{
    lab::SourceThread source;
    QSignalSpy spy(&source, &lab::SourceThread::frame_produced);

    source.start(20);
    ASSERT_TRUE(wait_for_count(spy, 2, 3000));

    source.stop();
    const int count_after_stop = spy.count();

    QTest::qWait(200);

    EXPECT_EQ(spy.count(), count_after_stop);
    EXPECT_FALSE(source.is_running());
}

TEST(FrameSourceTest, StopWithoutStartIsSafe)
{
    lab::SourceThread source;
    QSignalSpy spy(&source, &lab::SourceThread::frame_produced);

    EXPECT_NO_THROW(source.stop());

    EXPECT_FALSE(source.is_running());
    EXPECT_EQ(spy.count(), 0);
}

TEST(FrameSourceTest, RestartAfterStopProducesFramesAgain)
{
    lab::SourceThread source;
    QSignalSpy spy(&source, &lab::SourceThread::frame_produced);

    source.start(20);
    ASSERT_TRUE(wait_for_count(spy, 1, 3000));
    source.stop();

    const int count_after_stop = spy.count();

    source.start(20);
    ASSERT_TRUE(wait_for_count(spy, count_after_stop + 2, 3000));
    source.stop();

    EXPECT_FALSE(source.is_running());
}

int main(int argc, char** argv)
{
    // 必须在构造 QApplication 之前设置，确保测试走无界面后端。
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}