#include "../src/core/MediaClock.h"

#include <iostream>
#include <cmath>
#include <thread>
#include <chrono>

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL: " << name << " (" << #expr << ")" << std::endl; \
        testsFailed++; \
    } else { \
        testsPassed++; \
    } \
} while(0)

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void testWaitingAfterSet() {
    MediaClock c;
    c.set(10.0);
    sleepMs(50);
    TEST("set() freezes clock until update()", std::abs(c.now() - 10.0) < 1e-9);
    TEST("isWaiting after set", c.isWaiting());
    TEST("not running while waiting", !c.isRunning());
}

void testUpdateRuns() {
    MediaClock c;
    c.update(5.0, 1.0);
    TEST("not waiting after update", !c.isWaiting());
    sleepMs(100);
    double now = c.now();
    TEST("clock advances in real time", now > 5.08 && now < 5.5);
}

void testSpeed() {
    MediaClock c;
    c.update(0.0, 2.0);
    sleepMs(100);
    double now = c.now();
    TEST("2x speed advances ~2x", now > 0.16 && now < 0.8);
}

void testPauseResume() {
    MediaClock c;
    c.update(1.0, 1.0);
    sleepMs(20);
    c.pause();
    double p = c.now();
    sleepMs(50);
    TEST("paused clock does not advance", std::abs(c.now() - p) < 1e-6);
    TEST("isPaused", c.isPaused());
    c.resume();
    sleepMs(50);
    TEST("resumed clock advances", c.now() > p + 0.04);
}

void testFreeze() {
    MediaClock c;
    c.update(2.0, 1.0);
    sleepMs(20);
    c.freeze();
    double f = c.now();
    sleepMs(50);
    TEST("frozen clock holds position", std::abs(c.now() - f) < 1e-6);
    TEST("frozen is not waiting", !c.isWaiting());
    c.update(3.0, 1.0);
    sleepMs(20);
    TEST("update after freeze resumes", c.now() > 3.0);
}

void testSetSpeedContinuity() {
    MediaClock c;
    c.update(0.0, 1.0);
    sleepMs(100);
    double before = c.now();
    c.setSpeed(2.0);
    double after = c.now();
    TEST("setSpeed keeps position continuous", std::abs(after - before) < 0.01);
    sleepMs(100);
    double delta = c.now() - after;
    TEST("new speed applies", delta > 0.16 && delta < 0.8);
}

void testUpdateWhilePaused() {
    MediaClock c;
    c.update(1.0, 1.0);
    c.pause();
    c.update(7.0, 1.0);   // 消费端在暂停态锚定（设备缓冲排空）
    TEST("update while paused stores position", std::abs(c.now() - 7.0) < 1e-9);
    sleepMs(30);
    TEST("still paused", std::abs(c.now() - 7.0) < 1e-9);
    c.resume();
    sleepMs(50);
    TEST("runs from stored position after resume", c.now() > 7.04 && c.now() < 7.5);
}

void testSetWhilePausedThenResumeStaysWaiting() {
    MediaClock c;
    c.update(1.0, 1.0);
    c.pause();
    c.set(4.0);          // 暂停态 seek
    c.resume();
    sleepMs(30);
    TEST("set() + resume() stays at target until data arrives", std::abs(c.now() - 4.0) < 1e-9);
    c.update(4.0, 1.0);
    sleepMs(30);
    TEST("first update starts the clock", c.now() > 4.02);
}

int main() {
    testWaitingAfterSet();
    testUpdateRuns();
    testSpeed();
    testPauseResume();
    testFreeze();
    testSetSpeedContinuity();
    testUpdateWhilePaused();
    testSetWhilePausedThenResumeStaysWaiting();

    std::cout << "MediaClock tests: " << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;
    return testsFailed > 0 ? 1 : 0;
}
