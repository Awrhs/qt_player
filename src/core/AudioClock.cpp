#include "AudioClock.h"

#include <algorithm>

// ── AudioClock ───────────────────────────────────────────────────────────

double AudioClock::now() const {
    int64_t samples = consumedSamples_.load(std::memory_order_acquire);
    int sr = sampleRate_.load(std::memory_order_acquire);
    double pts = pts_.load(std::memory_order_acquire);
    if (sr <= 0) return pts;
    return pts + static_cast<double>(samples) / sr;
}

void AudioClock::set(double pts) {
    pts_.store(pts, std::memory_order_release);
    consumedSamples_.store(0, std::memory_order_release);
}

void AudioClock::addConsumedSamples(int64_t samples, int sampleRate) {
    sampleRate_.store(sampleRate, std::memory_order_release);
    consumedSamples_.fetch_add(samples, std::memory_order_acq_rel);
}

void AudioClock::reset() {
    set(0.0);
}

// ── ExternalClock ────────────────────────────────────────────────────────

ExternalClock::ExternalClock()
    : baseTime_(std::chrono::steady_clock::now())
{
}

double ExternalClock::now() const {
    if (paused_) return basePts_;
    auto elapsed = std::chrono::steady_clock::now() - baseTime_;
    double sec = std::chrono::duration<double>(elapsed).count();
    return basePts_ + sec * speed_;
}

void ExternalClock::set(double pts) {
    basePts_ = pts;
    baseTime_ = std::chrono::steady_clock::now();
}

void ExternalClock::setSpeed(double speed) {
    speed = std::clamp(speed, 0.5, 2.0);
    if (paused_) {
        speed_ = speed;
        return;
    }
    // 保持连续性：将当前时间折算到新基准
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - baseTime_).count();
    basePts_ = basePts_ + elapsed * speed_;
    baseTime_ = now;
    speed_ = speed;
}

void ExternalClock::pause() {
    // 记录暂停时刻的 PTS
    basePts_ = now();
    paused_ = true;
}

void ExternalClock::resume() {
    baseTime_ = std::chrono::steady_clock::now();
    paused_ = false;
}

void ExternalClock::reset() {
    set(0.0);
    paused_ = false;
}