#include "../src/core/SpectrumAnalyzer.h"

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

void testSingleTone() {
    SpectrumAnalyzer analyzer;

    // 生成 1kHz 正弦波（16kHz 采样率）
    constexpr int sampleRate = 16000;
    constexpr double freq = 1000.0;
    std::vector<float> signal(2048);
    for (int i = 0; i < 2048; ++i) {
        signal[i] = std::sin(2.0 * M_PI * freq * i / sampleRate);
    }

    analyzer.feed(signal.data(), 2048);
    auto bands = analyzer.bands();

    // 1kHz 应该在 24 段中的第 8~12 段之间（步进对数）
    // 简单验证：至少有若干非零频段
    int nonZero = 0;
    for (auto b : bands) {
        if (b > 0.01f) nonZero++;
    }
    TEST("Single tone produces non-zero bands", nonZero > 0);
    TEST("Bands count is 24", bands.size() == 24);
}

void testSilence() {
    SpectrumAnalyzer analyzer;

    std::vector<float> signal(2048, 0.0f);
    analyzer.feed(signal.data(), 2048);
    auto bands = analyzer.bands();

    // 静音应该导致所有频段接近 0
    int nearZero = 0;
    for (auto b : bands) {
        if (b < 0.1f) nearZero++;
    }
    TEST("Silence produces near-zero bands", nearZero > 20);
}

void testMultipleTones() {
    SpectrumAnalyzer analyzer;

    // 多频率组合
    constexpr int sampleRate = 16000;
    std::vector<float> signal(4096);
    for (int i = 0; i < 4096; ++i) {
        signal[i] = std::sin(2.0 * M_PI * 440.0 * i / sampleRate)  // A4
                  + std::sin(2.0 * M_PI * 880.0 * i / sampleRate); // A5
    }

    // 分两次输入，覆盖内部累积
    analyzer.feed(signal.data(), 2048);
    analyzer.feed(signal.data() + 2048, 2048);

    auto bands = analyzer.bands();

    bool hasContent = false;
    for (auto b : bands) {
        if (b > 0.1f) hasContent = true;
    }
    TEST("Multiple tones produce content", hasContent);
}

int main() {
    testSingleTone();
    testSilence();
    testMultipleTones();

    std::cout << "FFT tests: "
              << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;

    return testsFailed > 0 ? 1 : 0;
}