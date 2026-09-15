#include "AudioOutput.h"
#include "AudioClock.h"
#include "Equalizer.h"
#include "SpectrumAnalyzer.h"
#include "../utils/Log.h"

#include <algorithm>
#include <cstring>

LOG_TAG("AudioOut")

// ══════════════════════════════════════════════════════════════════════════
//  AudioIO
// ══════════════════════════════════════════════════════════════════════════

AudioOutput::AudioIO::AudioIO(AudioOutput* output)
    : QIODevice(output), output_(output) {}

qint64 AudioOutput::AudioIO::readData(char* data, qint64 maxSize) {
    if (!output_ || !output_->running_) return 0;

    size_t requestedFrames = maxSize / (sizeof(float) * output_->outputChannels_);
    if (requestedFrames == 0) return 0;

    float* out = reinterpret_cast<float*>(data);
    size_t totalFrames = 0;

    while (totalFrames < requestedFrames) {
        std::vector<float> frame;
        if (!output_->pcmQueue_.pop(frame, std::chrono::milliseconds(0))) {
            break;
        }

        size_t frameFrames = frame.size() / output_->outputChannels_;
        size_t copyFrames = std::min(frameFrames, requestedFrames - totalFrames);
        size_t copySamples = copyFrames * output_->outputChannels_;

        std::memcpy(out + totalFrames * output_->outputChannels_,
                    frame.data(), copySamples * sizeof(float));

        totalFrames += copyFrames;

        if (copyFrames < frameFrames) {
            LOG_W("Partial frame discard: %zu/%zu frames", copyFrames, frameFrames);
        }
    }

    // 静音填充
    size_t remainingFrames = requestedFrames - totalFrames;
    if (remainingFrames > 0) {
        std::memset(out + totalFrames * output_->outputChannels_, 0,
                    remainingFrames * output_->outputChannels_ * sizeof(float));
        output_->underrunCount_.fetch_add(1);
        if (output_->underrunCount_.load() <= 1 || output_->underrunCount_.load() % 10 == 0) {
            LOG_W("Audio underrun: %d total", output_->underrunCount_.load());
            emit output_->underrun();
        }
    }

    // 更新时钟
    output_->consumedSamples_.fetch_add(totalFrames);
    if (output_->audioClock_) {
        output_->audioClock_->addConsumedSamples(totalFrames, output_->outputSampleRate_);
    }

    return static_cast<qint64>(totalFrames * output_->outputChannels_ * sizeof(float));
}

qint64 AudioOutput::AudioIO::writeData(const char* /*data*/, qint64 /*maxSize*/) {
    return 0;
}

// ══════════════════════════════════════════════════════════════════════════
//  AudioOutput
// ══════════════════════════════════════════════════════════════════════════

AudioOutput::AudioOutput(QObject* parent)
    : QObject(parent) {}

AudioOutput::~AudioOutput() {
    stop();
}

bool AudioOutput::init(int sampleRate, int channels, uint64_t channelLayout, AudioClock* audioClock) {
    stop();

    audioClock_ = audioClock;

    // 保存源参数
    srcSampleRate_ = sampleRate;
    srcChannels_   = channels;
    srcLayout_     = channelLayout;

    // 输出参数
    outputSampleRate_ = 44100;
    outputChannels_   = 2;
    outputLayout_     = AV_CH_LAYOUT_STEREO;

    // 创建重采样上下文
    swrCtx_ = swr_alloc_set_opts(nullptr,
                                  outputLayout_,     AV_SAMPLE_FMT_FLT, outputSampleRate_,
                                  channelLayout ? channelLayout : av_get_default_channel_layout(channels),
                                  (AVSampleFormat)AV_SAMPLE_FMT_FLT, sampleRate,
                                  0, nullptr);
    if (!swrCtx_) {
        LOG_E("swr_alloc_set_opts failed");
        return false;
    }

    int ret = swr_init(swrCtx_);
    if (ret < 0) {
        LOG_E("swr_init failed: %d", ret);
        swr_free(&swrCtx_);
        return false;
    }

    // 创建音频设备
    QAudioFormat fmt;
    fmt.setSampleRate(outputSampleRate_);
    fmt.setChannelCount(outputChannels_);
    fmt.setSampleFormat(QAudioFormat::Float);

    audioSink_ = std::make_unique<QAudioSink>(fmt, this);
    audioIO_   = new AudioIO(this);

    initialized_ = true;
    LOG_I("AudioOutput initialized: %dHz %dch FLT", outputSampleRate_, outputChannels_);
    return true;
}

void AudioOutput::start() {
    if (!initialized_.load()) return;
    running_ = true;
    audioIO_->open(QIODevice::ReadOnly);
    audioSink_->start(audioIO_);
    LOG_I("AudioOutput started");
}

void AudioOutput::stop() {
    running_ = false;

    if (audioSink_) {
        audioSink_->stop();
    }
    if (audioIO_) {
        audioIO_->close();
    }

    clear();

    if (swrCtx_) {
        swr_free(&swrCtx_);
        swrCtx_ = nullptr;
    }

    initialized_ = false;
    consumedSamples_ = 0;
    underrunCount_ = 0;
    LOG_I("AudioOutput stopped");
}

void AudioOutput::pause() {
    if (audioSink_) {
        audioSink_->suspend();
    }
    running_ = false;
}

void AudioOutput::resume() {
    running_ = true;
    if (audioSink_) {
        audioSink_->resume();
    }
}

void AudioOutput::writeFrames(const float* data, size_t frames) {
    if (!running_ || !initialized_.load()) return;

    size_t samples = frames * outputChannels_;
    std::vector<float> frame(data, data + samples);

    // 均衡器处理
    if (equalizer_) {
        equalizer_->process(frame.data(), frames);
    }

    // 频谱分析（EQ 之后，PCM 入队之前）
    if (spectrumAnalyzer_) {
        std::vector<float> mono(frames);
        for (size_t i = 0; i < frames; ++i) {
            float sum = 0.0f;
            for (int ch = 0; ch < outputChannels_; ++ch) {
                sum += frame[i * outputChannels_ + ch];
            }
            mono[i] = sum / outputChannels_;
        }
        spectrumAnalyzer_->feed(mono.data(), frames);
    }

    if (!pcmQueue_.push(std::move(frame), std::chrono::milliseconds(500))) {
        LOG_W("PCM buffer full, dropping audio frame");
    }
}

bool AudioOutput::writeFrame(AVFrame* frame) {
    if (!initialized_.load() || !swrCtx_) return false;

    uint8_t* output[1] = {nullptr};
    int outSamples = swr_get_out_samples(swrCtx_, frame->nb_samples);
    if (outSamples <= 0) {
        LOG_E("swr_get_out_samples returned %d", outSamples);
        return false;
    }

    // 分配输出缓冲
    av_samples_alloc(output, nullptr, outputChannels_, outSamples,
                     AV_SAMPLE_FMT_FLT, 0);
    if (!output[0]) return false;

    // 重采样
    int actualSamples = swr_convert(swrCtx_, output, outSamples,
                                     (const uint8_t**)frame->data, frame->nb_samples);
    if (actualSamples < 0) {
        LOG_E("swr_convert failed: %d", actualSamples);
        av_freep(&output[0]);
        return false;
    }

    if (actualSamples > 0) {
        writeFrames(reinterpret_cast<const float*>(output[0]), actualSamples);
    }

    av_freep(&output[0]);
    return true;
}

void AudioOutput::clear() {
    pcmQueue_.clear();
    consumedSamples_ = 0;
}