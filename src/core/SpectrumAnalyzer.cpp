#include "SpectrumAnalyzer.h"
#include "../utils/Log.h"

#include <cmath>
#include <algorithm>
#include <cstring>

LOG_TAG("Spectrum")

SpectrumAnalyzer::SpectrumAnalyzer()
    : bands_(NUM_BANDS, 0.0f)
{
    buffer_.reserve(FFT_SIZE);
}

void SpectrumAnalyzer::feed(const float* data, size_t samples) {
    for (size_t i = 0; i < samples; ++i) {
        buffer_.push_back(data[i]);
        if (buffer_.size() >= FFT_SIZE) {
            processNow();
            buffer_.clear();
        }
    }
}

std::vector<float> SpectrumAnalyzer::processNow() {
    // 确保有足够数据
    size_t n = std::min(buffer_.size(), static_cast<size_t>(FFT_SIZE));
    if (n < 4) return bands_;

    // 准备 FFT 输入（padding 到 FFT_SIZE）
    std::vector<double> real(FFT_SIZE, 0.0);
    std::vector<double> imag(FFT_SIZE, 0.0);

    for (size_t i = 0; i < n; ++i) {
        real[i] = static_cast<double>(buffer_[i]);
    }

    // Hann 窗
    applyHannWindow(real, FFT_SIZE);

    // FFT
    fft(real, imag, FFT_SIZE);

    // 幅度谱（取前 N/2 个 bin）
    std::vector<double> magnitude(FFT_SIZE / 2, 0.0);
    for (int i = 0; i < FFT_SIZE / 2; ++i) {
        magnitude[i] = std::sqrt(real[i] * real[i] + imag[i] * imag[i]);
    }

    // 对数分档：24 段
    // 频段范围：约 20Hz ~ 8000Hz（16kHz 采样率下 FFT 可到 8kHz）
    static const double bandFreqs[NUM_BANDS + 1] = {
        20, 33, 56, 94, 159, 268, 451, 761, 1280, 2154,
        3627, 6108, 8000  // +1 作为上边界
    };

    // 调整实际使用的段数（8000Hz 以上我们截断）
    const int usableBands = NUM_BANDS;
    std::vector<double> bandEnergy(usableBands, 0.0);
    std::vector<int> bandCount(usableBands, 0);

    int halfFft = FFT_SIZE / 2;
    for (int bin = 1; bin < halfFft; ++bin) {
        double freq = static_cast<double>(bin) * SAMPLE_RATE / FFT_SIZE;
        // 找到所属频段
        for (int b = 0; b < usableBands; ++b) {
            if (freq >= bandFreqs[b] && freq < bandFreqs[b + 1]) {
                bandEnergy[b] += magnitude[bin];
                bandCount[b]++;
                break;
            }
        }
    }

    // 平均 + dB 归一化到 [0, 1]
    double maxEnergy = 1e-6;
    for (int b = 0; b < usableBands; ++b) {
        if (bandCount[b] > 0) {
            bandEnergy[b] /= bandCount[b];
        }
        // dB 转换
        bands_[b] = static_cast<float>(20.0 * std::log10(bandEnergy[b] + 1e-6));
        bands_[b] = std::max(0.0f, bands_[b]); // 0dB 以上视为 1（实际罕见）
        // 归一化到 [0,1]
        bands_[b] = std::min(1.0f, bands_[b] / 60.0f);
        if (bands_[b] < 0) bands_[b] = 0;
        maxEnergy = std::max(maxEnergy, static_cast<double>(bands_[b]));
    }

    // 自动增益：确保有峰值
    if (maxEnergy > 0) {
        // 平滑
        for (int b = 0; b < usableBands; ++b) {
            bands_[b] = static_cast<float>(bands_[b] / maxEnergy);
        }
    }

    return bands_;
}

void SpectrumAnalyzer::applyHannWindow(std::vector<double>& buffer, int n) {
    for (int i = 0; i < n; ++i) {
        double w = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (n - 1)));
        buffer[i] *= w;
    }
}

void SpectrumAnalyzer::fft(std::vector<double>& real, std::vector<double>& imag, int n) {
    // 位反转重排
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    // 蝶形运算
    for (int len = 2; len <= n; len <<= 1) {
        double angle = -2.0 * M_PI / len;
        double wReal = std::cos(angle);
        double wImag = std::sin(angle);

        for (int i = 0; i < n; i += len) {
            double uReal = 1.0, uImag = 0.0;
            for (int j = 0; j < len / 2; ++j) {
                int even = i + j;
                int odd  = i + j + len / 2;

                // t = w * a[odd]
                double tReal = uReal * real[odd] - uImag * imag[odd];
                double tImag = uReal * imag[odd] + uImag * real[odd];

                real[odd] = real[even] - tReal;
                imag[odd] = imag[even] - tImag;
                real[even] += tReal;
                imag[even] += tImag;

                // 旋转因子递进
                double tmp = uReal * wReal - uImag * wImag;
                uImag = uReal * wImag + uImag * wReal;
                uReal = tmp;
            }
        }
    }
}

int SpectrumAnalyzer::freqToBin(double freq, int fftSize, int sampleRate) const {
    return static_cast<int>(freq * fftSize / sampleRate);
}