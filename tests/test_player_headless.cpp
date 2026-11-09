// 端到端集成测试：在无显示器 / 无声卡环境中驱动 PlayerController。
// 用 ffmpeg 命令行生成短测试片（找不到 ffmpeg 时返回 77 = 跳过）。

#include "../src/core/PlayerController.h"

#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QFileInfo>

#include <iostream>
#include <functional>
#include <cmath>
#include <cstdlib>

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL: " << name << " (" << #expr << ")" << std::endl; \
        testsFailed++; \
    } else { \
        std::cout << "  ok: " << name << std::endl; \
        testsPassed++; \
    } \
} while(0)

static void pump(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
}

static bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
    return true;
}

struct Probe {
    int       frames   = 0;
    double    lastFrame = -1.0;
    int       finished = 0;
    PlayState state    = PlayState::Idle;
    QString   lastError;

    explicit Probe(PlayerController& c) {
        QObject::connect(&c, &PlayerController::frameDisplayed, [this](double pts) {
            frames++;
            lastFrame = pts;
        });
        QObject::connect(&c, &PlayerController::stateChanged, [this](PlayState s) { state = s; });
        QObject::connect(&c, &PlayerController::playbackFinished, [this] { finished++; });
        QObject::connect(&c, &PlayerController::errorOccurred, [this](const QString& e) {
            lastError = e;
            std::cerr << "  error signal: " << e.toStdString() << std::endl;
        });
    }
};

static bool generate(const QString& ffmpeg, const QStringList& args) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(ffmpeg, args);
    if (!p.waitForStarted(20000)) {
        std::cerr << "ffmpeg failed to start" << std::endl;
        return false;
    }
    if (!p.waitForFinished(180000)) {
        std::cerr << "ffmpeg timed out" << std::endl;
        return false;
    }
    if (p.exitCode() != 0) {
        std::cerr << "ffmpeg failed: " << p.readAll().toStdString() << std::endl;
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        std::cerr << "SKIP: ffmpeg executable not found" << std::endl;
        return 77;
    }

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::cerr << "cannot create temp dir" << std::endl;
        return 1;
    }
    const QString avPath = dir.filePath("av.mp4");
    const QString vPath  = dir.filePath("v.mp4");
    const QString aPath  = dir.filePath("a.m4a");

    const QStringList common = {"-y", "-hide_banner", "-loglevel", "error", "-nostdin"};
    if (!generate(ffmpeg, QStringList(common) << "-f" << "lavfi" << "-i" << "testsrc2=size=320x240:rate=25"
                                            << "-f" << "lavfi" << "-i" << "sine=frequency=440:sample_rate=48000"
                                            << "-t" << "4" << "-c:v" << "mpeg4" << "-q:v" << "4" << "-g" << "12"
                                            << "-c:a" << "aac" << "-b:a" << "96k" << avPath) ||
        !generate(ffmpeg, QStringList(common) << "-f" << "lavfi" << "-i" << "testsrc2=size=320x240:rate=25"
                                            << "-t" << "4" << "-c:v" << "mpeg4" << "-q:v" << "4" << "-g" << "12" << vPath) ||
        !generate(ffmpeg, QStringList(common) << "-f" << "lavfi" << "-i" << "sine=frequency=440:sample_rate=48000"
                                            << "-t" << "4" << "-c:a" << "aac" << "-b:a" << "96k" << aPath)) {
        std::cerr << "SKIP: could not generate test media" << std::endl;
        return 77;
    }

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        std::cerr << "WATCHDOG: test hung" << std::endl;
        std::_Exit(3);
    });
    watchdog.start(240000);

    PlayerController player;
    Probe probe(player);
    const double frameDur = 1.0 / 25.0;

    // ════════════════════ 音视频文件 ════════════════════
    std::cout << "== A/V file ==" << std::endl;
    player.open(avPath);
    TEST("open: state Playing", player.state() == PlayState::Playing);
    TEST("open: has video and audio", player.hasVideo() && player.hasAudio());
    TEST("open: duration ~4s", std::fabs(player.duration() - 4.0) < 0.3);
    TEST("open: first frame displayed", waitFor([&] { return probe.frames >= 1; }, 8000));
    TEST("open: position advances", waitFor([&] { return player.position() > 0.5; }, 8000));
    std::cout << "  audio sink: " << (player.audioUsingNullSink() ? "null (no device)" : "device") << std::endl;
    TEST("play: frames keep coming", waitFor([&] { return probe.frames >= 10; }, 8000));

    // seek 播放中
    probe.lastFrame = -1.0;
    player.seek(2.0);
    TEST("seek: frame near target displayed", waitFor([&] { return probe.lastFrame >= 1.9; }, 8000));
    TEST("seek: first frame is the one containing target", probe.lastFrame < 2.0 + 2 * frameDur + 0.001);
    pump(300);
    {
        double pos = player.position();
        TEST("seek: position near target", pos > 1.9 && pos < 2.9);
    }
    TEST("seek: still playing", player.state() == PlayState::Playing);

    // 暂停
    player.pause();
    pump(100);
    {
        double p1 = player.position();
        pump(300);
        TEST("pause: state Paused", player.state() == PlayState::Paused);
        TEST("pause: position frozen", std::fabs(player.position() - p1) < 0.05);
    }

    // 暂停态 seek：保持暂停并显示目标帧
    probe.lastFrame = -1.0;
    player.seek(1.0);
    TEST("paused seek: target frame displayed", waitFor([&] { return probe.lastFrame >= 0.0; }, 8000));
    TEST("paused seek: frame contains target",
         probe.lastFrame <= 1.0 + 0.001 && probe.lastFrame > 1.0 - frameDur - 0.001);
    TEST("paused seek: still paused", player.state() == PlayState::Paused);
    pump(200);
    TEST("paused seek: position at target", std::fabs(player.position() - 1.0) < 0.1);

    // 逐帧前进 ×2
    for (int i = 0; i < 2; ++i) {
        double before = player.currentFramePts();
        player.stepForward();
        pump(50);
        double after = player.currentFramePts();
        TEST("step forward: exactly one frame", std::fabs((after - before) - frameDur) < 0.012);
        TEST("step forward: still paused", player.state() == PlayState::Paused);
    }
    // 逐帧后退
    {
        double before = player.currentFramePts();
        player.stepBackward();
        TEST("step back: previous frame displayed",
             waitFor([&] { return player.currentFramePts() < before - 0.02; }, 8000));
        double after = player.currentFramePts();
        TEST("step back: exactly one frame", std::fabs((before - after) - frameDur) < 0.012);
        TEST("step back: still paused", player.state() == PlayState::Paused);
    }

    // 恢复播放
    player.play();
    TEST("play: state Playing", waitFor([&] { return player.state() == PlayState::Playing; }, 2000));
    {
        double p2 = player.position();
        pump(700);
        TEST("play: position advances after resume", player.position() > p2 + 0.3);
    }

    // 倍速 2x
    player.setSpeed(2.0);
    TEST("speed: reported", std::fabs(player.speed() - 2.0) < 1e-9);
    pump(400);
    {
        int framesBefore = probe.frames;
        double p3 = player.position();
        pump(1000);
        double d = player.position() - p3;
        std::cout << "  2x: position delta over 1s = " << d << std::endl;
        TEST("speed: 2x advances ~2s per second", d > 1.3 && d < 2.8);
        TEST("speed: frames still displayed at 2x", probe.frames > framesBefore + 5);
    }

    // 自然结束
    probe.finished = 0;
    player.seek(3.4);
    TEST("end: playbackFinished emitted", waitFor([&] { return probe.finished > 0; }, 10000));
    TEST("end: state Paused", player.state() == PlayState::Paused);
    TEST("end: isEnded", player.isEnded());
    TEST("end: position at duration", player.position() > player.duration() - 0.3);
    TEST("end: no error", probe.lastError.isEmpty());

    // 循环播放
    player.setLoop(true);
    TEST("loop: flag set", player.loop());
    probe.finished = 0;
    player.play();
    TEST("loop: play at end restarts", waitFor([&] { return player.state() == PlayState::Playing && player.position() < 1.0; }, 3000));
    TEST("loop: frames after restart", waitFor([&] { int n = probe.frames; return n > 0 && probe.lastFrame < 1.0; }, 5000));
    player.seek(3.5);
    TEST("loop: wraps around to start",
         waitFor([&] { return player.position() < 1.5 && !player.isEnded() &&
                              player.state() == PlayState::Playing; }, 10000));
    pump(400);
    TEST("loop: still playing after wrap", player.state() == PlayState::Playing && probe.finished == 0);
    TEST("loop: frames displayed after wrap", probe.lastFrame >= 0.0 && probe.lastFrame < 2.0);
    player.setLoop(false);

    // 停止
    player.stop();
    TEST("stop: state Stopped", player.state() == PlayState::Stopped);

    // ════════════════════ 纯视频文件 ════════════════════
    std::cout << "== video-only file ==" << std::endl;
    probe.frames = 0;
    probe.lastFrame = -1.0;
    probe.finished = 0;
    player.setSpeed(2.0);
    player.open(vPath);
    TEST("video-only: Playing", player.state() == PlayState::Playing);
    TEST("video-only: streams", player.hasVideo() && !player.hasAudio());
    TEST("video-only: frames", waitFor([&] { return probe.frames >= 5; }, 8000));
    TEST("video-only: position advances", waitFor([&] { return player.position() > 0.5; }, 8000));
    probe.lastFrame = -1.0;
    player.seek(2.0);
    TEST("video-only: seek frame", waitFor([&] { return probe.lastFrame >= 1.9; }, 8000));
    TEST("video-only: finishes", waitFor([&] { return probe.finished > 0; }, 10000));
    TEST("video-only: ended state", player.isEnded() && player.state() == PlayState::Paused);

    // ════════════════════ 纯音频文件 ════════════════════
    std::cout << "== audio-only file ==" << std::endl;
    probe.finished = 0;
    player.setSpeed(1.0);
    player.open(aPath);
    TEST("audio-only: Playing", player.state() == PlayState::Playing);
    TEST("audio-only: streams", !player.hasVideo() && player.hasAudio());
    TEST("audio-only: position advances", waitFor([&] { return player.position() > 0.5; }, 8000));
    player.pause();
    pump(100);
    {
        double p = player.position();
        pump(200);
        TEST("audio-only: pause freezes position", std::fabs(player.position() - p) < 0.05);
    }
    player.seek(3.5);
    TEST("audio-only: paused seek keeps paused", player.state() == PlayState::Paused);
    pump(200);
    TEST("audio-only: paused seek position", std::fabs(player.position() - 3.5) < 0.1);
    player.play();
    TEST("audio-only: finishes", waitFor([&] { return probe.finished > 0; }, 10000));

    // 关闭
    player.shutdown();
    TEST("shutdown: no error", probe.lastError.isEmpty());

    std::cout << "PlayerHeadless tests: " << testsPassed << " passed, "
              << testsFailed << " failed, dropped frames (last media): "
              << player.droppedFrames() << std::endl;
    return testsFailed > 0 ? 1 : 0;
}
