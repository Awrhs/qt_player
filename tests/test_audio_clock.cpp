#include "../src/core/AudioClock.h"

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

void testBasicClock() {
    AudioClock clock;

    clock.set(10.0);
    double now = clock.now();
    TEST("Clock starts at set point", std::abs(now - 10.0) < 0.001);
}

void testSamplesAdvance() {
    AudioClock clock;

    clock.set(0.0);
    clock.addConsumedSamples(44100, 44100);
    double now = clock.now();
    TEST("1 second of samples advances by 1s", std::abs(now - 1.0) < 0.001);

    clock.addConsumedSamples(44100, 44100);
    now = clock.now();
    TEST("2 seconds total", std::abs(now - 2.0) < 0.001);
}

void testReset() {
    AudioClock clock;

    clock.set(5.0);
    clock.addConsumedSamples(22050, 44100);
    clock.reset();
    double now = clock.now();
    TEST("Reset goes to 0", std::abs(now) < 0.001);
}

void testClockWithSetAndSamples() {
    AudioClock clock;

    clock.set(30.0);     // seek 到 30s
    clock.addConsumedSamples(4410, 44100); // 0.1s
    double now = clock.now();
    TEST("Clock = pts + consumed", std::abs(now - 30.1) < 0.001);
}

void testExternalClock() {
    ExternalClock clock;

    clock.set(0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    double now = clock.now();
    TEST("External clock advances with real time", now > 0.09 && now < 0.3);
}

void testExternalPause() {
    ExternalClock clock;

    clock.set(10.0);
    clock.pause();
    double paused = clock.now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    double afterSleep = clock.now();
    TEST("Paused clock does not advance",
         std::abs(afterSleep - paused) < 0.01);

    clock.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    double afterResume = clock.now();
    TEST("Resumed clock advances", afterResume > afterSleep + 0.04);
}

int main() {
    testBasicClock();
    testSamplesAdvance();
    testReset();
    testClockWithSetAndSamples();
    testExternalClock();
    testExternalPause();

    std::cout << "AudioClock tests: "
              << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;

    return testsFailed > 0 ? 1 : 0;
}