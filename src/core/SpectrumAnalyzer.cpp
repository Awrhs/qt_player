#include "SpectrumAnalyzer.h"

#include <cmath>
#include <algorithm>
#include <cstring>

SpectrumAnalyzer::SpectrumAnalyzer() {
    buffer_.reserve(FFT_SIZE);
    real_.resize(FFT_SIZE);
    imag_.resize(FFT_SIZE);
    window_.resize(FFT_SIZE);
    rebuildLayout();
}

void SpectrumAnalyzer::setSampleRate(int sampleRate) {
    if (sampleRate <= 0) return;
    if (sampleRate == sampleRate_) return;
    sampleRate_ = sampleRate;
    rebuildLayout();
}

void SpectrumAnalyzer::feed(const float* data, size_t samples) {
    for (size_t i = 0; i < samples; ++i) {
        pushMono(data[i]);
    }
}

void SpectrumAnalyzer::feedInterleaved(const float* data, size_t frames, int channels) {
    if (channels <= 0) return;
    if (channels == 1) {
        feed(data, frames);
        return;
    }
    for (size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) {
            sum += data[i * channels + c];
        }
        pushMono(sum / channels);
    }
}

void SpectrumAnalyzer::pushMono(float sample) {
    buffer_.push_back(sample);
    if (buffer_.size() >= FFT_SIZE) {
        processNow();
        buffer_.clear();
    }
}

std::vector<float> SpectrumAnalyzer::bands() const {
    std::lock_guard<std::mutex> lock(bandsMtx_);
    return std::vector<float>(bands_.begin(), bands_.end());
}

void SpectrumAnalyzer::reset() {
    buffer_.clear();
    std::lock_guard<std::mutex> lock(bandsMtx_);
    bands_.fill(0.0f);
}

void SpectrumAnalyzer::rebuildLayout() {
    // Hann 窗表（只算一次）
    for (int i = 0; i < FFT_SIZE; ++i) {
        window_[i] = static_cast<float>(
            0.5 * (1.0 - std::cos(2.0 * M_PI * i / (FFT_SIZE - 1))));
    }

    // 对数频段边界：20Hz ~ min(16kHz, 奈奎斯特的 90%)
    const double fStart = 20.0;
    const double fEnd   = std::min(16000.0, sampleRate_ * 0.5 * 0.9);
    std::array<double, NUM_BANDS + 1> edges{};
    for (int b = 0; b <= NUM_BANDS; ++b) {
        double t = static_cast<double>(b) / NUM_BANDS;
        edges[b] = fStart * std::pow(fEnd / fStart, t);
    }

    // 预构 bin→band 映射，processNow 不再做 O(N×bands) 查找
    binBand_.fill(-1);
    for (int bin = 1; bin < HALF_FFT; ++bin) {
        double freq = static_cast<double>(bin) * sampleRate_ / FFT_SIZE;
        for (int b = 0; b < NUM_BANDS; ++b) {
            if (freq >= edges[b] && freq < edges[b+1]) {
                binBand_[bin] = b;
                break;
            }
        }
    }
}

void SpectrumAnalyzer::processNow() {
    const size_t n = std::min(buffer_.size(), static_cast<size_t>(FFT_SIZE));
    if (n < 4) return;

    // 零填充到 FFT_SIZE（复用成员缓冲）
    std::fill(real_.begin(), real_.end(), 0.0f);
    std::fill(imag_.begin(), imag_.end(), 0.0f);
    for (size_t i = 0; i < n; ++i) {
        real_[i] = buffer_[i] * window_[i];
    }

    fft(real_.data(), imag_.data(), FFT_SIZE);

    // 按预构 LUT 累加各频段幅度
    std::array<float, NUM_BANDS> bandEnergy{};
    std::array<int,   NUM_BANDS> bandCount{};
    for (int bin = 1; bin < HALF_FFT; ++bin) {
        int b = binBand_[bin];
        if (b < 0) continue;
        float re = real_[bin], im = imag_[bin];
        bandEnergy[b] += std::sqrt(re * re + im * im);
        bandCount[b]++;
    }

    // 平均 + dB 归一化到 [0,1]，再做自动增益（保持原有显示特性）
    std::array<float, NUM_BANDS> result{};
    float maxEnergy = 1e-6f;
    for (int b = 0; b < NUM_BANDS; ++b) {
        float v = 0.0f;
        if (bandCount[b] > 0) {
            v = bandEnergy[b] / bandCount[b];
        }
        v = static_cast<float>(20.0 * std::log10(v + 1e-6));
        v = std::max(0.0f, v);
        v = std::min(1.0f, v / 60.0f);
        result[b] = v;
        maxEnergy = std::max(maxEnergy, v);
    }
    if (maxEnergy > 0.0f) {
        for (int b = 0; b < NUM_BANDS; ++b) {
            result[b] /= maxEnergy;
        }
    }

    std::lock_guard<std::mutex> lock(bandsMtx_);
    bands_ = result;
}

void SpectrumAnalyzer::fft(float* real, float* imag, int n) {
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
        float angle = static_cast<float>(-2.0 * M_PI / len);
        float wReal = std::cos(angle);
        float wImag = std::sin(angle);

        for (int i = 0; i < n; i += len) {
            float uReal = 1.0f, uImag = 0.0f;
            for (int j = 0; j < len / 2; ++j) {
                int even = i + j;
                int odd  = i + j + len / 2;

                float tReal = uReal * real[odd] - uImag * imag[odd];
                float tImag = uReal * imag[odd] + uImag * real[odd];

                real[odd] = real[even] - tReal;
                imag[odd] = imag[even] - tImag;
                real[even] += tReal;
                imag[even] += tImag;

                float tmp = uReal * wReal - uImag * wImag;
                uImag = uReal * wImag + uImag * wReal;
                uReal = tmp;
            }
        }
    }
}
