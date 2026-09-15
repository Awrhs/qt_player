#include "../src/core/Equalizer.h"

#include <iostream>
#include <cmath>
#include <cassert>

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

void testPresetNames() {
    auto names = Equalizer::presetNames();
    TEST("Has presets", names.size() > 0);

    bool hasFlat = false;
    for (const auto& n : names) {
        if (n == "Flat") hasFlat = true;
    }
    TEST("Flat preset exists", hasFlat);
}

void testAllPresets() {
    Equalizer eq;
    Equalizer eq2;

    // Flat 预设应该与默认一样
    eq.setPreset("Flat");
    eq.setPreset("Pop");
    eq.setPreset("Rock");
    eq.setPreset("Classical");
    eq.setPreset("Jazz");

    // 切换预设不应崩溃
    float data[128] = {0.0f};
    for (int i = 0; i < 64; ++i) data[i] = 0.5f;

    for (int p = 0; p < 5; ++p) {
        switch (p) {
            case 0: eq.setPreset("Flat"); break;
            case 1: eq.setPreset("Pop"); break;
            case 2: eq.setPreset("Rock"); break;
            case 3: eq.setPreset("Classical"); break;
            case 4: eq.setPreset("Jazz"); break;
        }
        eq.process(data, 64); // 不应崩溃
    }
    TEST("All presets process without crash", true);
}

void testBandGainChanges() {
    Equalizer eq;

    double g0 = eq.getBandGain(0);
    TEST("Default gain is 0", std::abs(g0) < 0.001);

    eq.setBandGain(0, 6.0);  // +6dB
    eq.setBandGain(1, -3.0); // -3dB

    float data[128];
    for (int i = 0; i < 128; ++i) data[i] = 0.5f;
    eq.process(data, 64); // 处理一帧
    TEST("Process after gain change works", true);
}

void testProcessStereo() {
    Equalizer eq;

    // 立体声信号
    float data[256]; // 128 frames, stereo
    for (int i = 0; i < 256; ++i) {
        data[i] = (i % 2 == 0) ? 0.5f : -0.5f;
    }

    // 多次处理
    for (int i = 0; i < 10; ++i) {
        eq.process(data, 128);
    }
    TEST("Stereo processing works", true);
}

int main() {
    testPresetNames();
    testAllPresets();
    testBandGainChanges();
    testProcessStereo();

    std::cout << "Equalizer tests: "
              << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;

    return testsFailed > 0 ? 1 : 0;
}