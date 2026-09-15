#ifndef EQUALIZER_H
#define EQUALIZER_H

#include <vector>
#include <string>
#include <array>
#include <map>

/**
 * @brief 10 段音频均衡器
 *
 * 基于 RBJ peaking EQ 公式，10 个 biquad（二阶 IIR）级联。
 * 中心频率（ISO）：31.25, 62.5, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz
 * 增益范围：±12dB
 *
 * 预设：Flat / Pop / Rock / Classical / Jazz
 */
class Equalizer {
public:
    static constexpr int NUM_BANDS = 10;

    Equalizer();

    /**
     * @brief 设置某频段增益（dB）
     * @param bandIndex 0~9
     * @param dB        -12 ~ +12 dB
     */
    void setBandGain(int bandIndex, double dB);

    /**
     * @brief 获取某频段增益
     */
    double getBandGain(int bandIndex) const;

    /**
     * @brief 切换预设
     * @param name "Flat", "Pop", "Rock", "Classical", "Jazz"
     */
    void setPreset(const std::string& name);

    /**
     * @brief 获取预设名称列表
     */
    static std::vector<std::string> presetNames();

    /**
     * @brief 处理 PCM 数据（就地，通道交错 float）
     * @param buf   float 缓冲
     * @param frames 帧数（每帧 = NUM_CHANNELS 个样本）
     */
    void process(float* buf, size_t frames);

    /** @brief 重置滤波状态（seek 时调用） */
    void clear();

    /** @brief 设置采样率 */
    void setSampleRate(int sampleRate) { sampleRate_ = sampleRate; }

private:
    /** @brief Biquad 滤波器结构体 */
    struct Biquad {
        // 系数（归一化到 a0=1）
        double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        // 延迟单元（每个通道一组）
        std::vector<double> x1, x2, y1, y2;

        void reset(int channels);
        void process(float* buf, size_t frames, int channels);
    };

    /**
     * @brief 计算 RBJ peaking EQ 系数
     * @param freq      中心频率（Hz）
     * @param dB        增益（dB）
     * @param Q         品质因数（默认 0.707）
     * @param sampleRate 采样率
     */
    static Biquad calcPeakingEQ(double freq, double dB, double Q, double sampleRate);

    /** @brief 预设表 */
    static const std::map<std::string, std::array<double, NUM_BANDS>> PRESETS;

    // ISO 中心频率
    static constexpr double BAND_FREQS[NUM_BANDS] = {
        31.25, 62.5, 125.0, 250.0, 500.0,
        1000.0, 2000.0, 4000.0, 8000.0, 16000.0
    };

    // 10 段 biquad
    std::array<Biquad, NUM_BANDS> biquads_;

    // 当前增益（dB）
    std::array<double, NUM_BANDS> gains_ = {0.0};

    // 目标增益（渐变用）
    std::array<double, NUM_BANDS> targetGains_ = {0.0};

    // 采样率
    int sampleRate_ = 44100;

    // 渐变状态
    bool ramping_ = false;
    int rampFrames_ = 0;
    int rampTotal_ = 0; // 10ms = sampleRate_ / 100 帧
};

#endif // EQUALIZER_H