#include "../src/core/AudioOutput.h"
#include "../src/core/MediaClock.h"

#include <QCoreApplication>
#include <QThread>

#include <iostream>
#include <cmath>
#include <thread>
#include <atomic>
#include <chrono>

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL: " << name << " (" << #expr << ")" << std::endl; \
        testsFailed++; \
    } else { \
        testsPassed++; \
    } \
} while(0)

static AVFrame* makeFrame(int nb, int rate, int64_t pts) {
    AVFrame* f = av_frame_alloc();
    f->nb_samples  = nb;
    f->format      = AV_SAMPLE_FMT_FLTP;
    f->sample_rate = rate;
    av_channel_layout_default(&f->ch_layout, 2);
    f->pts = pts;
    av_frame_get_buffer(f, 0);
    for (int ch = 0; ch < 2; ++ch) {
        float* d = reinterpret_cast<float*>(f->data[ch]);
        for (int i = 0; i < nb; ++i) {
            d[i] = 0.3f * std::sin(2.0 * M_PI * 440.0 * (pts + i) / rate);
        }
    }
    return f;
}

static void pushFrames(AudioOutput& out, uint64_t serial, int count, int64_t firstPts) {
    for (int i = 0; i < count; ++i) {
        AVFrame* f = makeFrame(1024, 48000, firstPts + i * 1024);
        out.pushFrame(f, serial);
        av_frame_free(&f);
    }
}

static bool near(double a, double b, double eps) { return std::fabs(a - b) < eps; }

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    MediaClock clock;
    AudioOutput out;
    out.setForceNullSink(true);
    out.setLatencyOverride(0.0);

    AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
    TEST("open", out.open(48000, &layout, AV_SAMPLE_FMT_FLTP, AVRational{1, 48000}, &clock));
    TEST("device rate = source rate", out.sampleRate() == 48000);
    out.flush(1, -1.0);
    clock.set(0.0);
    TEST("serial after flush", out.currentSerial() == 1);

    std::vector<float> buf(48000 * 2);

    // 1. 推送 → 拉取 → 时钟锚定
    pushFrames(out, 1, 10, 0);
    TEST("frames queued", out.queuedFrames() > 0);
    out.pullAudio(buf.data(), 2400);
    TEST("clock anchored to delivered audio (0.05s)", near(clock.now(), 0.05, 0.01));
    TEST("clock running", !clock.isWaiting());
    TEST("pulled data non-silent", std::fabs(buf[100]) > 0.01f || std::fabs(buf[101]) > 0.01f);

    // 2. 数据饥饿 → 时钟停表
    for (int i = 0; i < 10 && out.underrunCount() == 0; ++i) out.pullAudio(buf.data(), 4800);
    TEST("underrun detected when starved", out.underrunCount() > 0);
    double frozen = clock.now();
    QThread::msleep(30);
    TEST("clock frozen while starving", near(clock.now(), frozen, 0.002));
    TEST("clock not running while starving", !clock.isRunning());

    // 3. seek：新代际 + pre-roll 裁剪；旧代际帧被丢弃；flush 自身把时钟置到目标点
    out.flush(2, 0.5);
    TEST("flush positions clock at target", near(clock.now(), 0.5, 1e-9) && clock.isWaiting());
    TEST("flush empties queue", out.queuedFrames() == 0 && !out.isDrained());
    pushFrames(out, 1, 3, 0);
    TEST("stale-serial frames dropped", out.queuedFrames() == 0);
    pushFrames(out, 2, 10, 19200);           // 0.4s 起，跨越 0.5s
    TEST("new-serial frames queued", out.queuedFrames() > 0);
    out.pullAudio(buf.data(), 2400);
    TEST("clock = seek target + delivered (0.55s)", near(clock.now(), 0.55, 0.01));

    // 4. 设备延迟补偿：时钟 = 交付末尾 − 延迟，且不早于本段起点
    out.flush(3, -1.0);
    clock.set(1.0);
    out.setLatencyOverride(0.05);
    pushFrames(out, 3, 10, 48000);
    out.pullAudio(buf.data(), 2400);
    TEST("latency compensation floors at segment start", near(clock.now(), 1.0, 0.005));
    out.pullAudio(buf.data(), 2400);
    TEST("latency compensation applied", near(clock.now(), 1.05, 0.005));
    out.setLatencyOverride(0.0);

    // 5. 倍速：PCM 携带 tempo，时钟按媒体时间推进
    out.flush(4, -1.0);
    clock.set(2.0);
    out.setSpeed(2.0);
    pushFrames(out, 4, 20, 96000);
    out.pullAudio(buf.data(), 4800);
    TEST("2x: 0.1s of device time = 0.2s of media time", near(clock.now(), 2.2, 0.02));
    out.setSpeed(1.0);

    // 6. EOF：排空后时钟继续走时（不停表）
    out.flush(5, -1.0);
    clock.set(3.0);
    pushFrames(out, 5, 5, 144000);
    out.pushEndOfStream(5);
    TEST("eos flagged", out.endOfStream());
    for (int i = 0; i < 20 && !out.isDrained(); ++i) out.pullAudio(buf.data(), 4800);
    TEST("drained after eos", out.isDrained());
    out.pullAudio(buf.data(), 4800);
    double atEnd = clock.now();
    QThread::msleep(40);
    TEST("clock keeps running after eos", clock.now() > atEnd + 0.02);

    // 7. EOF 时没有任何音频（seek 到音频末尾之后）：waiting 时钟改为自走
    out.flush(6, -1.0);
    clock.set(4.0);
    TEST("clock waiting after seek", clock.isWaiting());
    out.pushEndOfStream(6);
    TEST("eos without data un-waits clock", !clock.isWaiting());
    QThread::msleep(30);
    TEST("clock free-runs", clock.now() > 4.02);

    // 7b. 最后一次拉取只拿到部分数据（时钟停表、非 waiting）后到达 EOF：时钟须恢复走时
    out.flush(8, -1.0);
    clock.set(5.0);
    pushFrames(out, 8, 3, 240000);            // atempo 需约 2000 样本后才有输出
    out.pullAudio(buf.data(), 4800);          // 只拿到部分输出，其余补静音 → 停表
    TEST("partial pull froze clock", !clock.isRunning() && !clock.isWaiting());
    out.pushEndOfStream(8);
    for (int i = 0; i < 5 && !out.isDrained(); ++i) out.pullAudio(buf.data(), 4800);
    out.pullAudio(buf.data(), 4800);
    TEST("eos un-freezes clock", clock.isRunning());
    {
        double before = clock.now();
        QThread::msleep(30);
        TEST("clock runs to the end after eos", clock.now() > before + 0.02);
    }

    // 8. 生产者阻塞在满队列上时，requestStop 能唤醒它
    out.flush(7, -1.0);
    std::atomic<bool> running{true};
    out.setStopToken(&running);
    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        pushFrames(out, 7, 200, 0);   // 远超 250ms 容量 → 阻塞
        producerDone = true;
    });
    QThread::msleep(150);
    TEST("producer blocked on full queue", !producerDone.load());
    auto t0 = std::chrono::steady_clock::now();
    out.requestStop();
    producer.join();
    double woke = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    TEST("requestStop wakes blocked producer", woke < 2.0);

    out.close();
    TEST("closed", !out.isOpen());

    std::cout << "AudioOutput tests: " << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;
    return testsFailed > 0 ? 1 : 0;
}
