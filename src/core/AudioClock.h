#ifndef AUDIO_CLOCK_H
#define AUDIO_CLOCK_H

#include <atomic>
#include <chrono>

/**
 * @brief 音频主时钟
 *
 * 基于音频设备已消耗的样本数计算当前播放位置。
 * 公式：clock = pts + consumedSamples / sampleRate
 *
 * 无音频流时使用 ExternalClock（基于 steady_clock 的递进时钟）。
 */
class AudioClock {
public:
    AudioClock() = default;

    /**
     * @brief 获取当前时钟值（秒）
     *
     * 有音频流时：基于已消耗样本计算。
     * 无音频流且使用 ExternalClock 时：基于 steady_clock 计算。
     */
    double now() const;

    /** @brief 设置时钟基准时间（seek 后调用） */
    void set(double pts);

    /**
     * @brief 增加已消耗样本数（音频输出回调中调用）
     * @param samples    本次消耗的样本数（每通道）
     * @param sampleRate 采样率（Hz）
     */
    void addConsumedSamples(int64_t samples, int sampleRate);

    /** @brief 重置时钟 */
    void reset();

private:
    std::atomic<double> pts_{0.0};                // 基准 PTS（秒）
    std::atomic<int64_t> consumedSamples_{0};     // 累积消耗样本数
    std::atomic<int>     sampleRate_{44100};      // 采样率
};

/**
 * @brief 外部时钟（无音频流时使用）
 *
 * 基于 std::chrono::steady_clock，按系统时间递增。
 */
class ExternalClock {
public:
    ExternalClock();

    /** @brief 获取当前时钟值（秒） */
    double now() const;

    /** @brief 设置基准时间 */
    void set(double pts);

    /** @brief 设置播放速度 */
    void setSpeed(double speed);

    /** @brief 暂停时停止时钟递增 */
    void pause();

    /** @brief 恢复时钟递增 */
    void resume();

    /** @brief 重置 */
    void reset();

private:
    std::chrono::steady_clock::time_point baseTime_;
    double basePts_ = 0.0;
    double speed_ = 1.0;
    bool paused_ = false;
};

#endif // AUDIO_CLOCK_H