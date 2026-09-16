#ifndef SPECTRUM_ANALYZER_H
#define SPECTRUM_ANALYZER_H

#include <vector>
#include <array>
#include <mutex>
#include <cstdint>

/**
 * @brief 频谱分析器
 *
 * 输入：单声道或交错多声道 PCM float 数据（采样率可配置，默认 48kHz）
 * 流程：Hann 窗 → 2048 点基-2 FFT(float) → 幅度谱 → 24 段对数分档 → dB 归一化
 * 输出：24 个 [0, 1] 频段值
 *
 * 线程模型：feed/feedInterleaved/processNow 只在音频解码线程调用；
 * bands() 可在 UI 线程并发读取，内部加锁返回拷贝。
 * setSampleRate 仅在 open（工作线程启动前）调用。
 *
 * FFT 使用自实现迭代式基-2 Cooley-Tukey 算法。
 */
class SpectrumAnalyzer {
public:
    static constexpr int FFT_SIZE  = 2048;        // 2^11
    static constexpr int NUM_BANDS = 24;
    static constexpr int HALF_FFT  = FFT_SIZE / 2;

    SpectrumAnalyzer();

    /**
     * @brief 输入单声道 PCM
     * @param data    输入数据
     * @param samples 样本数
     * 内部累积到 FFT_SIZE 后自动处理。
     */
    void feed(const float* data, size_t samples);

    /**
     * @brief 输入交错多声道 PCM，内部下混为单声道（无中间缓冲分配）
     * @param data     交错数据（帧内每声道一个样本）
     * @param frames   帧数
     * @param channels 声道数
     */
    void feedInterleaved(const float* data, size_t frames, int channels);

    /** @brief 设置输入采样率（重建频段映射，线程启动前调用） */
    void setSampleRate(int sampleRate);

    /**
     * @brief 获取频段值（线程安全）
     * @return 24 个 [0, 1] 归一化值
     */
    std::vector<float> bands() const;

    /** @brief 重置累积缓冲与输出 */
    void reset();

private:
    /** @brief 累积满 FFT_SIZE 后执行一次分析 */
    void processNow();

    /** @brief 喂入一个已下混的单声道样本 */
    void pushMono(float sample);

    /** @brief 按当前采样率重建 Hann 窗与 bin→频段映射 */
    void rebuildLayout();

    /** @brief 迭代式基-2 FFT（in/out，n 必须是 2 的幂） */
    void fft(float* real, float* imag, int n);

    int sampleRate_ = 48000;

    // 输入累积（单声道）
    std::vector<float> buffer_;

    // FFT 复用缓冲（构造时一次性分配）
    std::vector<float> real_;
    std::vector<float> imag_;
    std::vector<float> window_;      // Hann 窗表

    // bin → 频段映射（-1 表示该 bin 不属于任何显示频段）
    std::array<int, HALF_FFT> binBand_{};

    // 输出（音频线程写、UI 线程读）
    mutable std::mutex bandsMtx_;
    std::array<float, NUM_BANDS> bands_{};
};

#endif // SPECTRUM_ANALYZER_H
