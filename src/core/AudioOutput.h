#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <QObject>
#include <QIODevice>
#include <QAudioSink>
#include <QByteArray>
#include <QPointer>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include "BoundedQueue.h"

class AudioClock;
class Equalizer;       // forward declaration
class SpectrumAnalyzer; // forward declaration

/**
 * @brief 音频输出模块
 *
 * 使用 QAudioSink 输出音频，包含：
 *  - swr_convert：源格式 → FLT / 立体声（采样率与源一致直通，由音频
 *    服务端决定是否重采样，避免 48k→44.1k→48k 双重重采样）
 *  - PCM 环形缓冲：解码线程填充，QAudioSink 消费
 *  - 欠载补齐：缓冲瞬时不足时短等待，仍不足则填静音并返回完整请求
 */
class AudioOutput : public QObject {
    Q_OBJECT
public:
    explicit AudioOutput(QObject* parent = nullptr);
    ~AudioOutput() override;

    /**
     * @brief 初始化音频输出
     * @param sampleRate       源采样率（输出采样率与源一致）
     * @param srcLayout        源声道布局（nullptr 时按立体声默认处理）
     * @param srcFmt           源采样格式（如 AAC 解码输出 FLTP）
     * @param audioClock       关联的音频时钟
     * @return 成功返回 true
     */
    bool init(int sampleRate, const AVChannelLayout* srcLayout,
              AVSampleFormat srcFmt, AudioClock* audioClock);

    /** @brief 启动音频输出 */
    void start();

    /**
     * @brief 停止音频输出并释放全部资源（调用方须保证生产线程已停止或不并发）
     */
    void stop();

    /**
     * @brief 关闭流程第一步（主线程）：停设备回调、推进 PCM 代际以唤醒任何
     *        阻塞在 writeFrames 的生产者，但保留 swr/QAudioSink 对象。
     *        之后调用方 join 生产线程，再调 finishShutdown() 释放资源。
     */
    void beginShutdown();

    /** @brief 关闭流程第二步：生产线程 join 后释放 swr/设备等资源 */
    void finishShutdown();

    /** @brief 暂停音频输出 */
    void pause();

    /** @brief 恢复音频输出 */
    void resume();

    /**
     * @brief 写入 PCM 数据（解码线程调用）
     * @param data    float 格式 PCM 数据（通道交错）
     * @param frames  帧数（每帧 channels 个样本）
     */
    void writeFrames(const float* data, size_t frames);

    /**
     * @brief 写入解码后的 AVFrame（自动重采样）
     * @param frame 解码后的音频帧
     * @return 成功返回 true
     */
    bool writeFrame(AVFrame* frame);

    /**
     * @brief 排空重采样器内部延迟样本（EOF drain 后由解码线程调用）
     *
     * swr 重采样内部缓存少量样本，正常 swr_convert 不会输出，
     * 必须以 NULL 输入再调一次才能取出，否则结尾会丢失几十毫秒声音。
     */
    void flushTail();

    /**
     * @brief 重置重采样管线（seek 后调用，清空 swr 内部延迟）
     */
    void resetPipeline();

    /** 清空缓冲（seek 时调用） */
    void clear();

    /** @brief PCM 队列与余量是否都已空（播放结束判定用） */
    bool isBufferEmpty() const;

    /** @brief PCM 队列代际（解码线程据此感知 seek/stop 清空） */
    uint64_t queueGeneration() const { return queueGeneration_.load(); }

    /** @brief 设置均衡器（可选） */
    void setEqualizer(Equalizer* eq) { equalizer_ = eq; }

    /** @brief 设置频谱分析器（可选） */
    void setSpectrumAnalyzer(SpectrumAnalyzer* sa) { spectrumAnalyzer_ = sa; }

    /** @brief 是否已初始化 */
    bool isInitialized() const { return initialized_.load(); }

    /** @brief 获取已消耗的样本数 */
    int64_t consumedSamples() const { return consumedSamples_.load(); }

    /** @brief 输出参数 */
    int outputSampleRate()  const { return outputSampleRate_; }
    int outputChannels()    const { return outputChannels_; }

    /**
     * @brief 设置音量（UI 线程调用）
     * @param percent 0 ~ 100
     */
    void setVolume(int percent);

    /**
     * @brief 静音开关（UI 线程调用）
     * @param muted true 静音
     */
    void setMuted(bool muted);

    /** @brief 当前音量 */
    int  volume() const { return volumePercent_.load(); }
    bool muted()  const { return muted_.load(); }

    /**
     * @brief 设置播放速度（通过改变输出采样率实现，变调不变速）
     * @param speed 0.5 ~ 2.0
     *
     * 原理：保持源解码器输出不变，改变 swr 输出采样率 = 源采样率 × speed。
     * 输出采样率变化 → QAudioSink 以新速率播放 → 等效变速。
     * 代价：每次速度变更需销毁并重建音频设备，会产生极短暂的停顿。
     */
    void setPlaybackSpeed(double speed);

signals:
    void underrun(); // 缓冲不足

private:
    /** @brief 将音量/静音状态应用到音频设备 */
    void applyVolume();

    /**
     * @brief 内部 QIODevice 实现
     *
     * QAudioSink 通过 readData 回调拉取数据。
     */
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

    // 内部 PCM 缓冲
    BoundedQueue<std::vector<float>> pcmQueue_{60};

    // readData 未消费完的 PCM 余量（设备回调线程读写，clear 时其他线程清）。
    // 用起始偏移消费，避免 vector 头部 erase 的 O(N) memmove。
    mutable std::mutex leftoverMutex_;
    std::vector<float> leftover_;
    size_t             leftoverOffset_ = 0;

    // 重采样
    SwrContext* swrCtx_ = nullptr;
    int outputSampleRate_  = 48000;
    int outputChannels_    = 2;

    // swr 输出缓冲（仅音频解码线程访问，容量只增不减，避免每帧分配）
    std::vector<float> swrBuf_;

    // 源参数
    int srcSampleRate_   = 48000;
    int srcChannels_     = 2;
    AVSampleFormat srcFmt_ = AV_SAMPLE_FMT_FLT;

    // 音频设备
    std::unique_ptr<QAudioSink> audioSink_;
    AudioIO*                    audioIO_ = nullptr;

    // 时钟
    AudioClock* audioClock_ = nullptr;

    // 均衡器（可选引用，不拥有）
    Equalizer* equalizer_ = nullptr;

    // 频谱分析器（可选引用，不拥有）
    SpectrumAnalyzer* spectrumAnalyzer_ = nullptr;

    // 状态
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<int64_t> consumedSamples_{0};

    // PCM 队列代际：每次 clear()（seek/stop）自增。writeFrames 入队受阻时
    // 靠它区分"暂停（设备暂不消费，继续保帧等待）"与"队列已作废（丢弃在途帧）"
    std::atomic<uint64_t> queueGeneration_{0};

    // 音量（0~100）与静音
    std::atomic<int>  volumePercent_{100};
    std::atomic<bool> muted_{false};

    // 播放速度
    std::atomic<double> speed_{1.0};

    // 保护 swrCtx_ 与 swrBuf_（demux 线程 setPlaybackSpeed 与音频解码线程
    // writeFrame 可能并发访问；|mutex| 加在 writeFrame/flushTail/resetPipeline
    // 与 setPlaybackSpeed 上，无性能瓶颈——写入频率极低）
    mutable std::mutex swrMutex_;

    // 静音填充统计
    std::atomic<int> underrunCount_{0};
};

#endif // AUDIO_OUTPUT_H
