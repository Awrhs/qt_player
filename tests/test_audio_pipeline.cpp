#include "../src/core/AudioPipeline.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <iostream>
#include <cmath>
#include <vector>
#include <algorithm>

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

static AVFrame* makeFrame(int nb, int rate, AVSampleFormat fmt, int channels,
                          int64_t pts, int64_t startSample, double freq = 440.0) {
    AVFrame* f = av_frame_alloc();
    f->nb_samples  = nb;
    f->format      = fmt;
    f->sample_rate = rate;
    av_channel_layout_default(&f->ch_layout, channels);
    f->pts = pts;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    for (int i = 0; i < nb; ++i) {
        double v = 0.5 * std::sin(2.0 * M_PI * freq * (startSample + i) / rate);
        for (int ch = 0; ch < channels; ++ch) {
            if (fmt == AV_SAMPLE_FMT_FLTP) {
                reinterpret_cast<float*>(f->data[ch])[i] = static_cast<float>(v);
            } else if (fmt == AV_SAMPLE_FMT_S16) {
                reinterpret_cast<int16_t*>(f->data[0])[i * channels + ch] =
                    static_cast<int16_t>(v * 32767.0);
            }
        }
    }
    return f;
}

static size_t totalFrames(const std::vector<PcmChunk>& out) {
    size_t n = 0;
    for (const auto& c : out) n += c.frames(2);
    return n;
}

static float peak(const std::vector<PcmChunk>& out) {
    float m = 0.0f;
    for (const auto& c : out)
        for (float v : c.data) m = std::max(m, std::fabs(v));
    return m;
}

static bool contiguous(const std::vector<PcmChunk>& out, int rate) {
    for (size_t i = 1; i < out.size(); ++i) {
        double expected = out[i-1].pts + out[i-1].frames(2) * out[i-1].tempo / rate;
        if (std::fabs(out[i].pts - expected) > 1e-6) return false;
    }
    return true;
}

static void feedSine(AudioPipeline& p, std::vector<PcmChunk>& out, int frames, int nb,
                     int rate, AVSampleFormat fmt, int channels, int64_t firstPts) {
    for (int i = 0; i < frames; ++i) {
        AVFrame* f = makeFrame(nb, rate, fmt, channels, firstPts + i * nb, i * nb);
        TEST("process ok", p.process(f, out));
        av_frame_free(&f);
    }
}

void testPassthrough() {
    AudioPipeline p;
    p.configure(48000, AVRational{1, 48000});
    p.reset(-1.0);
    std::vector<PcmChunk> out;
    feedSine(p, out, 20, 1024, 48000, AV_SAMPLE_FMT_FLTP, 2, 0);
    TEST("flush ok", p.flush(out));

    const size_t n = totalFrames(out);
    TEST("passthrough count ~ input", n > 20480 * 0.88 && n < 20480 * 1.12);
    TEST("first chunk at pts 0", !out.empty() && std::fabs(out[0].pts) < 1e-6);
    TEST("chunks contiguous", contiguous(out, 48000));
    TEST("media position ~ input duration",
         std::fabs(p.mediaPosition() - 20480.0 / 48000.0) < 0.08);
    TEST("output not silent", peak(out) > 0.2f);
    TEST("stereo frames", !out.empty() && out[0].data.size() % 2 == 0);
}

void testTempo(double tempo) {
    AudioPipeline p;
    p.configure(48000, AVRational{1, 48000});
    p.reset(-1.0);
    TEST("setTempo ok", p.setTempo(tempo));
    std::vector<PcmChunk> out;
    feedSine(p, out, 20, 1024, 48000, AV_SAMPLE_FMT_FLTP, 2, 0);
    p.flush(out);

    const double expected = 20480.0 / tempo;
    const size_t n = totalFrames(out);
    std::cout << "  tempo " << tempo << ": " << n << " frames (expected ~" << expected << ")\n";
    TEST("tempo output count", n > expected * 0.88 && n < expected * 1.12);
    TEST("tempo recorded on chunks", !out.empty() && std::fabs(out[0].tempo - tempo) < 1e-9);
    TEST("media position independent of tempo",
         std::fabs(p.mediaPosition() - 20480.0 / 48000.0) < 0.1);
    TEST("chunks contiguous at tempo", contiguous(out, 48000));
}

void testTrim() {
    AudioPipeline p;
    p.configure(48000, AVRational{1, 48000});
    p.reset(0.25);   // seek 目标 0.25s
    std::vector<PcmChunk> out;

    // 完全早于目标点的帧不产生任何输出（甚至不构建滤镜图）
    feedSine(p, out, 11, 1024, 48000, AV_SAMPLE_FMT_FLTP, 2, 0);   // 到 0.2347s
    TEST("pre-roll frames produce no output", out.empty());

    for (int i = 11; i < 20; ++i) {
        AVFrame* f = makeFrame(1024, 48000, AV_SAMPLE_FMT_FLTP, 2, i * 1024, i * 1024);
        p.process(f, out);
        av_frame_free(&f);
    }
    p.flush(out);
    TEST("first chunk starts exactly at seek target", !out.empty() && std::fabs(out[0].pts - 0.25) < 1e-4);
    const double expected = (20480.0 / 48000.0 - 0.25) * 48000.0;
    const size_t n = totalFrames(out);
    TEST("trimmed output count", n > expected * 0.85 && n < expected * 1.15);
    TEST("media position reaches input end", std::fabs(p.mediaPosition() - 20480.0 / 48000.0) < 0.08);
}

void testFormatConversion() {
    AudioPipeline p;
    p.configure(48000, AVRational{1, 44100});
    p.reset(-1.0);
    std::vector<PcmChunk> out;
    feedSine(p, out, 20, 1024, 44100, AV_SAMPLE_FMT_S16, 1, 0);
    p.flush(out);
    const double expected = 20480.0 * 48000.0 / 44100.0;
    const size_t n = totalFrames(out);
    TEST("resampled count", n > expected * 0.88 && n < expected * 1.12);
    TEST("mono -> stereo", !out.empty() && out[0].data.size() == out[0].frames(2) * 2);
    TEST("media position in source seconds", std::fabs(p.mediaPosition() - 20480.0 / 44100.0) < 0.08);
    TEST("converted output not silent", peak(out) > 0.2f);
}

void testTempoChangeMidStream() {
    AudioPipeline p;
    p.configure(48000, AVRational{1, 48000});
    p.reset(-1.0);
    std::vector<PcmChunk> out;
    feedSine(p, out, 10, 1024, 48000, AV_SAMPLE_FMT_FLTP, 2, 0);
    TEST("live tempo change ok", p.setTempo(2.0));
    for (int i = 10; i < 20; ++i) {
        AVFrame* f = makeFrame(1024, 48000, AV_SAMPLE_FMT_FLTP, 2, i * 1024, i * 1024);
        p.process(f, out);
        av_frame_free(&f);
    }
    p.flush(out);
    const double expected = 10240.0 + 5120.0;
    const size_t n = totalFrames(out);
    std::cout << "  tempo change: " << n << " frames (expected ~" << expected << ")\n";
    TEST("mid-stream tempo change output count", n > expected * 0.85 && n < expected * 1.15);
    TEST("media position tracks input after tempo change",
         std::fabs(p.mediaPosition() - 20480.0 / 48000.0) < 0.12);
    TEST("chunks contiguous across tempo change", contiguous(out, 48000));
}

void testNoPts() {
    AudioPipeline p;
    p.configure(48000, AVRational{1, 48000});
    p.reset(-1.0);
    std::vector<PcmChunk> out;
    for (int i = 0; i < 10; ++i) {
        AVFrame* f = makeFrame(1024, 48000, AV_SAMPLE_FMT_FLTP, 2, AV_NOPTS_VALUE, i * 1024);
        TEST("process without pts", p.process(f, out));
        av_frame_free(&f);
    }
    p.flush(out);
    TEST("no-pts frames start at 0", !out.empty() && std::fabs(out[0].pts) < 1e-6);
    TEST("no-pts position advances", std::fabs(p.mediaPosition() - 10240.0 / 48000.0) < 0.08);
}

void testInputChangeRebuild() {
    AudioPipeline p;
    p.configure(48000, AVRational{1, 48000});
    p.reset(-1.0);
    std::vector<PcmChunk> out;
    feedSine(p, out, 5, 1024, 48000, AV_SAMPLE_FMT_FLTP, 2, 0);
    for (int i = 0; i < 5; ++i) {
        AVFrame* f = makeFrame(1024, 48000, AV_SAMPLE_FMT_S16, 1, (5 + i) * 1024, i * 1024);
        TEST("process after input format change", p.process(f, out));
        av_frame_free(&f);
    }
    p.flush(out);
    TEST("rebuild produces output", totalFrames(out) > 5000);
    TEST("rebuild keeps chunks contiguous", contiguous(out, 48000));
}

void testResetDropsState() {
    AudioPipeline p;
    p.configure(48000, AVRational{1, 48000});
    p.reset(-1.0);
    std::vector<PcmChunk> out;
    feedSine(p, out, 10, 1024, 48000, AV_SAMPLE_FMT_FLTP, 2, 0);
    out.clear();
    p.reset(5.0);   // seek 到 5s
    TEST("reset clears position", !p.hasPosition());
    AVFrame* f = makeFrame(1024, 48000, AV_SAMPLE_FMT_FLTP, 2, 48000 * 5, 0);
    p.process(f, out);
    av_frame_free(&f);
    p.flush(out);
    TEST("after reset first chunk at new position", !out.empty() && std::fabs(out[0].pts - 5.0) < 1e-4);
}

int main() {
    testPassthrough();
    testTempo(2.0);
    testTempo(0.5);
    testTempo(1.25);
    testTrim();
    testFormatConversion();
    testTempoChangeMidStream();
    testNoPts();
    testInputChangeRebuild();
    testResetDropsState();

    std::cout << "AudioPipeline tests: " << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;
    return testsFailed > 0 ? 1 : 0;
}
