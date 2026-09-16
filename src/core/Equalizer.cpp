#include "Equalizer.h"
#include "../utils/Log.h"

#include <cmath>
#include <algorithm>

LOG_TAG("Equalizer")

// 增益趋近时间常数 τ：约 20ms 内完成大部分过渡，既听不出切换痕迹，
// 又不会因为渐变太久导致预设切换后音色长时间偏离
static constexpr float kRampTauSec = 0.020f;
// 增益吸附阈值（dB），避免无限逼近造成的无效系数重算
static constexpr float kGainEpsDb = 0.005f;

// 预设表：10 段增益值（dB）
const std::map<std::string, std::array<double, Equalizer::NUM_BANDS>> Equalizer::PRESETS = {
    {"Flat",      {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Pop",       {5, 4, 3, 1, 0, 0, 1, 2, 3, 4}},
    {"Rock",      {4, 4, 2, 0, -1, -2, 1, 2, 3, 4}},
    {"Classical", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Jazz",      {2, 3, 3, 2, 1, 1, 0, 1, 2, 3}}
};

Equalizer::Equalizer() {
    for (auto& g : targetGains_) g.store(0.0f);
    currentGains_.fill(0.0f);
    // 初始即为正确的直通系数（b0=1），避免“全零系数导致无声”
    for (int i = 0; i < NUM_BANDS; ++i) {
        biquads_[i].setCoeffs(calcPeakingEQ(BAND_FREQS[i], 0.0, 0.707,
                                            activeSampleRate_));
        biquads_[i].ensureChannels(CHANNELS);
    }
}

void Equalizer::setSampleRate(int sampleRate) {
    if (sampleRate > 0) sampleRate_.store(sampleRate);
}

void Equalizer::setBandGain(int bandIndex, double dB) {
    if (bandIndex < 0 || bandIndex >= NUM_BANDS) return;
    dB = std::clamp(dB, -12.0, 12.0);
    targetGains_[bandIndex].store(static_cast<float>(dB),
                                  std::memory_order_release);
}

double Equalizer::getBandGain(int bandIndex) const {
    if (bandIndex < 0 || bandIndex >= NUM_BANDS) return 0.0;
    return targetGains_[bandIndex].load(std::memory_order_acquire);
}

void Equalizer::setPreset(const std::string& name) {
    auto it = PRESETS.find(name);
    if (it == PRESETS.end()) {
        LOG_W("Unknown preset: %s", name.c_str());
        return;
    }
    LOG_I("Loading preset: %s", name.c_str());
    const auto& gains = it->second;
    for (int i = 0; i < NUM_BANDS; ++i) {
        setBandGain(i, gains[i]);
    }
}

std::vector<std::string> Equalizer::presetNames() {
    std::vector<std::string> names;
    for (const auto& [name, _] : PRESETS) {
        names.push_back(name);
        (void)_;
    }
    return names;
}

void Equalizer::process(float* buf, size_t frames) {
    if (!buf || frames == 0) return;

    int sr = sampleRate_.load(std::memory_order_acquire);
    if (sr <= 0) sr = activeSampleRate_;

    // 采样率变化：重算全部段系数。延迟线保留——信号流连续，没有爆音。
    if (sr != activeSampleRate_) {
        activeSampleRate_ = sr;
        for (int b = 0; b < NUM_BANDS; ++b) {
            biquads_[b].setCoeffs(calcPeakingEQ(BAND_FREQS[b],
                                                currentGains_[b], 0.707, sr));
        }
    }

    // 当前增益向目标指数趋近；只在增益未到位时更新该段系数
    const float alpha = 1.0f - std::exp(-static_cast<float>(frames) /
                                         sr / kRampTauSec);
    for (int b = 0; b < NUM_BANDS; ++b) {
        float target = targetGains_[b].load(std::memory_order_acquire);
        float cur = currentGains_[b];
        if (cur == target) continue;
        if (std::fabs(target - cur) < kGainEpsDb) {
            cur = target;
        } else {
            cur += (target - cur) * alpha;
        }
        currentGains_[b] = cur;
        biquads_[b].setCoeffs(calcPeakingEQ(BAND_FREQS[b], cur, 0.707, sr));
    }

    // 级联 10 段 biquad（identity 段走零乘法快路径）
    for (int b = 0; b < NUM_BANDS; ++b) {
        biquads_[b].process(buf, frames, CHANNELS);
    }
}

void Equalizer::clear() {
    for (auto& g : targetGains_) g.store(0.0f);
    currentGains_.fill(0.0f);
    int sr = sampleRate_.load();
    if (sr <= 0) sr = activeSampleRate_;
    activeSampleRate_ = sr;
    for (int i = 0; i < NUM_BANDS; ++i) {
        biquads_[i].setCoeffs(calcPeakingEQ(BAND_FREQS[i], 0.0, 0.707, sr));
        biquads_[i].reset(CHANNELS);
    }
}

Equalizer::Coeffs Equalizer::calcPeakingEQ(double freq, double dB, double Q,
                                           double sampleRate) {
    Coeffs c;
    if (dB == 0.0) {
        // 0dB：直通
        c.b0 = 1.0; c.b1 = 0.0; c.b2 = 0.0;
        c.a1 = 0.0; c.a2 = 0.0;
        c.identity = true;
        return c;
    }

    double A = std::pow(10.0, dB / 40.0);
    double omega = 2.0 * M_PI * freq / sampleRate;
    double alpha = std::sin(omega) / (2.0 * Q);
    double cosOmega = std::cos(omega);

    // 归一化到 a0
    c.b0 =  (1.0 + alpha * A) / (1.0 + alpha / A);
    c.b1 = (-2.0 * cosOmega) / (1.0 + alpha / A);
    c.b2 =  (1.0 - alpha * A) / (1.0 + alpha / A);
    c.a1 = (-2.0 * cosOmega) / (1.0 + alpha / A);
    c.a2 =  (1.0 - alpha / A) / (1.0 + alpha / A);
    c.identity = false;
    return c;
}

void Equalizer::Biquad::ensureChannels(int channels) {
    if (static_cast<int>(x1.size()) != channels) reset(channels);
}

void Equalizer::Biquad::reset(int channels) {
    x1.assign(channels, 0.0);
    x2.assign(channels, 0.0);
    y1.assign(channels, 0.0);
    y2.assign(channels, 0.0);
}

void Equalizer::Biquad::process(float* buf, size_t frames, int channels) {
    if (c.identity) {
        // 直通快路径：输出=输入，但延迟状态线仍跟随信号旋转，
        // 这样拉离 0dB 时状态连续、无 click
        for (size_t i = 0; i < frames; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                int idx = i * channels + ch;
                double in = buf[idx];
                x2[ch] = x1[ch];
                x1[ch] = in;
                y2[ch] = y1[ch];
                y1[ch] = in;
            }
        }
        return;
    }

    const float b0 = static_cast<float>(c.b0);
    const float b1 = static_cast<float>(c.b1);
    const float b2 = static_cast<float>(c.b2);
    const float a1 = static_cast<float>(c.a1);
    const float a2 = static_cast<float>(c.a2);
    for (size_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            int idx = i * channels + ch;
            float in = buf[idx];
            float out = b0 * in
                      + b1 * static_cast<float>(x1[ch])
                      + b2 * static_cast<float>(x2[ch])
                      - a1 * static_cast<float>(y1[ch])
                      - a2 * static_cast<float>(y2[ch]);
            x2[ch] = x1[ch];
            x1[ch] = in;
            y2[ch] = y1[ch];
            y1[ch] = out;
            buf[idx] = out;
        }
    }
}
