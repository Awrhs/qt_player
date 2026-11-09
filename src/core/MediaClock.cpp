#include "MediaClock.h"

#include <algorithm>

using Clock = std::chrono::steady_clock;

double MediaClock::nowLocked() const {
    if (!running_ || paused_) return pos_;
    double elapsed = std::chrono::duration<double>(Clock::now() - t0_).count();
    return pos_ + elapsed * speed_;
}

double MediaClock::now() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return nowLocked();
}

void MediaClock::set(double pos) {
    std::lock_guard<std::mutex> lock(mtx_);
    pos_     = pos;
    running_ = false;
    waiting_ = true;
}

void MediaClock::update(double pos, double speed) {
    std::lock_guard<std::mutex> lock(mtx_);
    pos_ = pos;
    t0_  = Clock::now();
    if (speed > 0.0) speed_ = speed;
    running_ = true;
    waiting_ = false;
}

void MediaClock::freeze() {
    std::lock_guard<std::mutex> lock(mtx_);
    pos_     = nowLocked();
    running_ = false;
}

void MediaClock::pause() {
    std::lock_guard<std::mutex> lock(mtx_);
    pos_    = nowLocked();
    paused_ = true;
}

void MediaClock::resume() {
    std::lock_guard<std::mutex> lock(mtx_);
    t0_     = Clock::now();
    paused_ = false;
}

void MediaClock::setSpeed(double speed) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (speed <= 0.0) return;
    pos_   = nowLocked();
    t0_    = Clock::now();
    speed_ = speed;
}

double MediaClock::speed() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return speed_;
}

bool MediaClock::isWaiting() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return waiting_;
}

bool MediaClock::isPaused() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return paused_;
}

bool MediaClock::isRunning() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return running_ && !paused_;
}
