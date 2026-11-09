#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <QObject>
#include <QIODevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QTimer>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include "AudioPipeline.h"
#include "PcmQueue.h"

class MediaClock;
class Equalizer;
class SpectrumAnalyzer;

/**
 * @brief 音频输出
 *
 * 数据流：解码线程 pushFrame → AudioPipeline（格式转换 + 变速）→ EQ/频谱 →
 *        PcmQueue（约 250ms 有界，背压）→ 设备回调 pullAudio → QAudioSink
 *
 *  - 主时钟：每次向设备交付 PCM 后，用该 PCM 携带的媒体时间锚定 MediaClock，
 *    并扣除设备缓冲延迟（QAudioSink::bufferSize），seek/变速后时钟自动跟随真实播放位置。
 *  - 无可用音频设备（无声卡、CI、设备被占用）时自动退化为“空设备”：由定时器按
 *    实时速率消费 PCM 并驱动时钟，视频照常播放。
 *  - 代际 serial：flush(serial) 之后，旧代际的帧一律丢弃，杜绝 seek 后听到旧位置声音。
 *
 * 线程约束：open/close/start/pause/resume/setVolume/setMuted 仅主线程；
 *          pushFrame/pushEndOfStream 仅解码线程；flush/setSpeed 任意线程。
 */
class AudioOutput : public QObject {
    Q_OBJECT
public:
    explicit AudioOutput(QObject* parent = nullptr);
    ~AudioOutput() override;

    /**
     * @brief 打开（选择设备采样率、配置管线），不启动设备
     * @param timeBase 音频流 time_base（解码帧 pts 的单位）
     */
    bool open(int srcSampleRate, const AVChannelLayout* srcLayout, AVSampleFormat srcFmt,
              AVRational timeBase, MediaClock* clock);

    /** @brief 关闭并释放（调用方须保证生产线程已退出） */
    void close();

    /** @brief 启动设备（失败则退化为空设备）；幂等 */
    bool start();
    void pause();
    void resume();

    /** @brief 关闭流程第一步（任意线程）：唤醒阻塞在入队上的生产者 */
    void requestStop();

    /** @brief 生产者的停止令牌：为 false 时入队等待立即返回 */
    void setStopToken(const std::atomic<bool>* running) { running_ = running; }

    // ── 生产者（解码线程） ──
    void pushFrame(const AVFrame* frame, uint64_t serial);
    void pushEndOfStream(uint64_t serial);

    // ── 控制 ──
    /** @brief seek：切换代际、丢弃全部缓冲、设置 pre-roll 丢弃阈值（流时间，秒） */
    void flush(uint64_t serial, double discardUntilSec);
    void setSpeed(double speed);
    void setVolume(int percent);
    void setMuted(bool muted);
    int  volume() const { return volume_.load(); }
    bool muted()  const { return muted_.load(); }

    // ── 状态 ──
    bool isOpen()        const { return opened_.load(); }
    bool isStarted()     const { return started_.load(); }
    bool usingNullSink() const { return nullSink_.load(); }
    /** @brief 已收到 EOF 且全部 PCM 已交付设备 */
    bool isDrained() const;
    bool endOfStream() const { return eos_.load(); }
    int  sampleRate() const { return outRate_; }
    int  channels()   const { return AudioPipeline::kOutChannels; }
    double latencySeconds() const { return latencySec_; }
    uint64_t currentSerial() const;
    size_t queuedFrames() const { return queue_.queuedFrames(); }
    int underrunCount() const { return underrunCount_.load(); }

    void setEqualizer(Equalizer* eq) { equalizer_ = eq; }
    void setSpectrumAnalyzer(SpectrumAnalyzer* sa) { spectrumAnalyzer_ = sa; }

    /**
     * @brief 消费端：拉取 frames 帧交错 float（不足补静音）并推进主时钟
     *
     * 由设备回调 / 空设备定时器调用；测试可直接调用以模拟设备。
     */
    size_t pullAudio(float* out, size_t frames);

    // ── 测试钩子 ──
    void setForceNullSink(bool force) { forceNull_ = force; }
    void setLatencyOverride(double sec) { latencyOverride_ = sec; if (sec >= 0.0) latencySec_ = sec; }

signals:
    void underrun();

private:
    class AudioIO : public QIODevice {
    public:
        explicit AudioIO(AudioOutput* output);
    protected:
        qint64 readData(char* data, qint64 maxSize) override;
        qint64 writeData(const char* data, qint64 maxSize) override;
    private:
        AudioOutput* output_;
    };
    friend class AudioIO;

    void applyVolume();
    void nullTick();
    void queueChunks(std::vector<PcmChunk>& chunks, uint64_t gen);

    mutable std::mutex pipeMtx_;
    AudioPipeline  pipeline_;
    uint64_t       serial_ = 0;
    PcmQueue       queue_;
    MediaClock*    clock_ = nullptr;
    const std::atomic<bool>* running_ = nullptr;

    QAudioFormat                format_;
    std::unique_ptr<QAudioSink> sink_;
    AudioIO*                    io_ = nullptr;
    QTimer*                     nullTimer_ = nullptr;
    std::chrono::steady_clock::time_point nullLast_{};
    std::vector<float>          nullScratch_;

    int    outRate_         = 48000;
    double latencySec_      = 0.0;
    double latencyOverride_ = -1.0;
    bool   forceNull_       = false;

    std::atomic<bool> opened_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> nullSink_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> eos_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> haveFloor_{false};
    std::atomic<double> floorPos_{0.0};
    std::atomic<double> speed_{1.0};
    std::atomic<int>  volume_{100};
    std::atomic<bool> muted_{false};
    std::atomic<int>  underrunCount_{0};
    std::atomic<bool> pipeErrorLogged_{false};

    Equalizer*        equalizer_        = nullptr;
    SpectrumAnalyzer* spectrumAnalyzer_ = nullptr;
};

#endif // AUDIO_OUTPUT_H
