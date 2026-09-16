#ifndef EQUALIZER_H
#define EQUALIZER_H

#include <vector>
#include <string>
#include <array>
#include <map>
#include <atomic>

/**
 * @brief 10 段音频均衡器
 *
 * 基于 RBJ peaking EQ 公式，10 个 biquad（二阶 IIR）级联。
 * 中心频率（ISO）：31.25, 62.5, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz
 * 增益范围：±12dB
 *
 * 预设：Flat / Pop / Rock / Classical / Jazz
 *
 * 线程模型：
 *  - UI 线程只通过 setBandGain/setPreset 写“目标增益”原子量、
 *    getBandGain 读目标值；
 *  - 系数与 IIR 延迟状态只在 process（音频解码线程）中访问，增益变化时
 *    当前增益按 ~20ms 时间常数指数趋近目标并平滑更换系数，绝不中途清零
 *    延迟线（清零会造成信号不连续的 click 爆音）。
 */
class Equalizer {
public:
    static constexpr int NUM_BANDS = 10;
    static constexpr int CHANNELS  = 2;

    Equalizer();

    /**
     * @brief 设置某频段目标增益（dB），UI 线程调用
     * @param bandIndex 0~9
     * @param dB        -12 ~ +12 dB
     */
    void setBandGain(int bandIndex, double dB);

    /**
     * @brief 获取某频段目标增益（dB），UI 线程调用
     */
    double getBandGain(int bandIndex) const;

    /**
     * @brief 切换预设
     * @param name "Flat", "Pop", "Rock", "Classical", "Jazz"
     */
    void setPreset(const std::string& name);

    /** @brief 获取预设名称列表 */
    static std::vector<std::string> presetNames();

    /**
     * @brief 处理 PCM 数据（就地，立体声交错 float），仅音频解码线程调用
     * @param buf    float 缓冲
     * @param frames 帧数（每帧 = CHANNELS 个样本）
     */
    void process(float* buf, size_t frames);

    /**
     * @brief 重置增益目标与全部滤波状态（open 新文件、工作线程启动前调用）
     */
    void clear();

    /** @brief 设置采样率（音频线程启动前调用；运行中变化由 process 平滑接管） */
    void setSampleRate(int sampleRate);

private:
    /** @brief Biquad 系数（归一化到 a0=1） */
    struct Coeffs {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        bool   identity = true;   // 0dB 直通形态
    };

    /** @brief Biquad 滤波器：系数 + 每声道延迟单元 */
    struct Biquad {
        Coeffs c;
        std::vector<double> x1, x2, y1, y2;

        void ensureChannels(int channels);
        void setCoeffs(const Coeffs& next) { c = next; } // 不触碰延迟状态
        void reset(int channels);
        void process(float* buf, size_t frames, int channels);
    };

    /**
     * @brief 计算 RBJ peaking EQ 系数
     * @param freq       中心频率（Hz）
     * @param dB         增益（dB），0dB 返回 identity 直通
     * @param Q          品质因数（0.707）
     * @param sampleRate 采样率
     */
    static Coeffs calcPeakingEQ(double freq, double dB, double Q, double sampleRate);

    /** @brief 预设表 */
    static const std::map<std::string, std::array<double, NUM_BANDS>> PRESETS;

    // ISO 中心频率
    static constexpr double BAND_FREQS[NUM_BANDS] = {
        31.25, 62.5, 125.0, 250.0, 500.0,
        1000.0, 2000.0, 4000.0, 8000.0, 16000.0
    };

    // 10 段 biquad（仅音频线程访问）
    std::array<Biquad, NUM_BANDS> biquads_;
    std::array<float, NUM_BANDS>  currentGains_{};
    int activeSampleRate_ = 48000;

    // UI 线程写 / 音频线程读的目标
    std::array<std::atomic<float>, NUM_BANDS> targetGains_{};
    std::atomic<int> sampleRate_{48000};
};

#endif // EQUALIZER_H
