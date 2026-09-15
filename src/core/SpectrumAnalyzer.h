#ifndef SPECTRUM_ANALYZER_H
#define SPECTRUM_ANALYZER_H

#include <vector>
#include <complex>
#include <cstdint>

/**
 * @brief 频谱分析器
 *
 * 输入：单声道 PCM float 数据
 * 流程：Hann 窗 → 2048 点基-2 FFT → 幅度谱 → 24 段对数分档 → dB 归一化
 * 输出：24 个 [0, 1] 频段值
 *
 * FFT 使用自实现迭代式基-2 Cooley-Tukey 算法。
 */
class SpectrumAnalyzer {
public:
    static constexpr int FFT_SIZE = 2048;       // 2^11
    static constexpr int NUM_BANDS = 24;
    static constexpr int SAMPLE_RATE = 16000;   // 内部重采样到 16kHz 降低计算量

    SpectrumAnalyzer();

    /**
     * @brief 输入 PCM 数据（单声道 float）
     * @param data  输入数据指针
     * @param samples 样本数
     *
     * 内部累积到 FFT_SIZE 后自动处理。
     */
    void feed(const float* data, size_t samples);

    /**
     * @brief 获取频段值
     * @return 24 个 [0, 1] 归一化值
     */
    std::vector<float> bands() const { return bands_; }

    /**
     * @brief 立即处理累积数据并返回频段值
     */
    std::vector<float> processNow();

private:
    /**
     * @brief 执行 2048 点 FFT
     * @param real 实部（in/out）
     * @param imag 虚部（in/out，初始化为 0）
     * @param n    点数（必须是 2 的幂）
     */
    void fft(std::vector<double>& real, std::vector<double>& imag, int n);

    /**
     * @brief 计算 Hann 窗
     * @param buffer 输入数据
     * @param n 长度
     */
    void applyHannWindow(std::vector<double>& buffer, int n);

    /**
     * @brief 频率到 FFT bin 索引
     */
    int freqToBin(double freq, int fftSize, int sampleRate) const;

    // 内部缓冲（单声道累积）
    std::vector<float> buffer_;
    size_t bufferPos_ = 0;

    // 输出
    std::vector<float> bands_;
};

#endif // SPECTRUM_ANALYZER_H