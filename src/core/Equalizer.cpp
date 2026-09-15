#include "Equalizer.h"
#include "../utils/Log.h"

#include <cmath>
#include <algorithm>
#include <cstring>

LOG_TAG("Equalizer")

// 预设表：10 段增益值（dB）
const std::map<std::string, std::array<double, Equalizer::NUM_BANDS>> Equalizer::PRESETS = {
    {"Flat",      {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Pop",       {5, 4, 3, 1, 0, 0, 1, 2, 3, 4}},
    {"Rock",      {4, 4, 2, 0, -1, -2, 1, 2, 3, 4}},
    {"Classical", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"Jazz",      {2, 3, 3, 2, 1, 1, 0, 1, 2, 3}}
};

Equalizer::Equalizer() {
    int channels = 2; // 默认立体声
    for (auto& bq : biquads_) {
        bq.reset(channels);
    }
    rampTotal_ = sampleRate_ / 100; // 10ms
}

void Equalizer::setBandGain(int bandIndex, double dB) {
    if (bandIndex < 0 || bandIndex >= NUM_BANDS) return;
    dB = std::clamp(dB, -12.0, 12.0);
    targetGains_[bandIndex] = dB;
    ramping_ = true;
    rampFrames_ = 0;
}

double Equalizer::getBandGain(int bandIndex) const {
    if (bandIndex < 0 || bandIndex >= NUM_BANDS) return 0.0;
    return gains_[bandIndex];
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
    }
    return names;
}

void Equalizer::process(float* buf, size_t frames) {
    if (!buf || frames == 0) return;

    int channels = 2; // 立体声

    // 渐变处理
    if (ramping_) {
        rampFrames_ += frames;
        // 每 10ms 重新计算一次系数
        if (rampFrames_ >= rampTotal_) {
            ramping_ = false;
            for (int i = 0; i < NUM_BANDS; ++i) {
                gains_[i] = targetGains_[i];
                biquads_[i] = calcPeakingEQ(BAND_FREQS[i], gains_[i], 0.707, sampleRate_);
                biquads_[i].reset(channels);
            }
        }
    }

    // 级联 10 段 biquad
    for (int b = 0; b < NUM_BANDS; ++b) {
        biquads_[b].process(buf, frames, channels);
    }
}

void Equalizer::clear() {
    for (auto& bq : biquads_) {
        bq.reset(2);
    }
    gains_ = {0.0};
    targetGains_ = {0.0};
    ramping_ = false;
}

Equalizer::Biquad Equalizer::calcPeakingEQ(double freq, double dB, double Q, double sampleRate) {
    Biquad bq;
    if (dB == 0.0) {
        // 0dB：直通
        bq.b0 = 1.0; bq.b1 = 0.0; bq.b2 = 0.0;
        bq.a1 = 0.0; bq.a2 = 0.0;
        return bq;
    }

    double A = std::pow(10.0, dB / 40.0);
    double omega = 2.0 * M_PI * freq / sampleRate;
    double alpha = std::sin(omega) / (2.0 * Q);

    double cosOmega = std::cos(omega);

    // 归一化到 a0
    bq.b0 = (1.0 + alpha * A) / (1.0 + alpha / A);
    bq.b1 = (-2.0 * cosOmega) / (1.0 + alpha / A);
    bq.b2 = (1.0 - alpha * A) / (1.0 + alpha / A);
    bq.a1 = (-2.0 * cosOmega) / (1.0 + alpha / A);
    bq.a2 = (1.0 - alpha / A) / (1.0 + alpha / A);

    return bq;
}

void Equalizer::Biquad::reset(int channels) {
    x1.assign(channels, 0.0);
    x2.assign(channels, 0.0);
    y1.assign(channels, 0.0);
    y2.assign(channels, 0.0);
}

void Equalizer::Biquad::process(float* buf, size_t frames, int channels) {
    for (size_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            int idx = i * channels + ch;
            double in = buf[idx];
            double out = b0 * in + b1 * x1[ch] + b2 * x2[ch] - a1 * y1[ch] - a2 * y2[ch];
            x2[ch] = x1[ch];
            x1[ch] = in;
            y2[ch] = y1[ch];
            y1[ch] = out;
            buf[idx] = static_cast<float>(out);
        }
    }
}