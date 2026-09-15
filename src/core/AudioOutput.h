#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <QObject>
#include <QIODevice>
#include <QAudioSink>
#include <QByteArray>
#include <memory>
#include <vector>
#include <atomic>

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
 *  - swr_convert：重采样到统一格式（FLT, 44100Hz, stereo）
 *  - PCM 环形缓冲：解码线程填充，QAudioSink 消费
 *  - 静音填充：缓冲不足时防止设备欠载
 */
class AudioOutput : public QObject {
    Q_OBJECT
public:
    explicit AudioOutput(QObject* parent = nullptr);
    ~AudioOutput() override;

    /**
     * @brief 初始化音频输出
     * @param sampleRate       源采样率
     * @param channels         声道数
     * @param channelLayout    声道布局
     * @param audioClock       关联的音频时钟
     * @return 成功返回 true
     */
    bool init(int sampleRate, int channels, uint64_t channelLayout, AudioClock* audioClock);

    /** @brief 启动音频输出 */
    void start();

    /** @brief 停止音频输出 */
    void stop();

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

    /** @brief 清空缓冲（seek 时调用） */
    void clear();

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

signals:
    void underrun(); // 缓冲不足

private:
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

    // 重采样
    SwrContext* swrCtx_ = nullptr;
    int outputSampleRate_  = 44100;
    int outputChannels_    = 2;
    int64_t outputLayout_  = AV_CH_LAYOUT_STEREO;

    // 源参数
    int srcSampleRate_   = 44100;
    int srcChannels_     = 2;
    uint64_t srcLayout_  = AV_CH_LAYOUT_STEREO;

    // 音频设备
    std::unique_ptr<QAudioSink> audioSink_;
    AudioIO*                    audioIO_ = nullptr;

    // 时钟
    AudioClock* audioClock_ = nullptr;

    // 均衡器（可选引用，不拥有）
    Equalizer* equalizer_ = nullptr;

    // 频谱分析器（可选引用）
    SpectrumAnalyzer* spectrumAnalyzer_ = nullptr;

    // 状态
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<int64_t> consumedSamples_{0};

    // 静音填充统计
    std::atomic<int> underrunCount_{0};
};

#endif // AUDIO_OUTPUT_H