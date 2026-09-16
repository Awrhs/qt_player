#include "AudioOutput.h"
#include "AudioClock.h"
#include "Equalizer.h"
#include "SpectrumAnalyzer.h"
#include "../utils/Log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

LOG_TAG("AudioOut")

// ══════════════════════════════════════════════════════════════════════════
//  AudioIO
// ══════════════════════════════════════════════════════════════════════════

AudioOutput::AudioIO::AudioIO(AudioOutput* output)
    : QIODevice(output), output_(output) {}

qint64 AudioOutput::AudioIO::readData(char* data, qint64 maxSize) {
    if (!output_ || !output_->running_.load()) return 0;

    const int ch = output_->outputChannels_;
    const size_t requestedFrames = maxSize / (sizeof(float) * ch);
    if (requestedFrames == 0) return 0;

    float* out = reinterpret_cast<float*>(data);
    size_t totalFrames = 0;

    // 1. 先消费上次未读完的余量（偏移读取，无 memmove）
    {
        std::lock_guard<std::mutex> lock(output_->leftoverMutex_);
        if (!output_->leftover_.empty()) {
            size_t availSamples = output_->leftover_.size() - output_->leftoverOffset_;
            size_t availFrames = availSamples / ch;
            size_t copyFrames = std::min(availFrames, requestedFrames);
            std::memcpy(out,
                        output_->leftover_.data() + output_->leftoverOffset_,
                        copyFrames * ch * sizeof(float));
            totalFrames = copyFrames;
            output_->leftoverOffset_ += copyFrames * ch;
            if (output_->leftoverOffset_ >= output_->leftover_.size()) {
                output_->leftover_.clear();
                output_->leftoverOffset_ = 0;
            }
        }
    }

    // 2. 继续从 PCM 队列取（非阻塞：不在音频回调线程里等待）
    while (totalFrames < requestedFrames) {
        std::vector<float> frame;
        if (!output_->pcmQueue_.pop(frame, std::chrono::milliseconds(0))) {
            break;
        }

        size_t frameFrames = frame.size() / ch;
        size_t copyFrames = std::min(frameFrames, requestedFrames - totalFrames);
        size_t copySamples = copyFrames * ch;

        std::memcpy(out + totalFrames * ch, frame.data(),
                    copySamples * sizeof(float));
        totalFrames += copyFrames;

        if (copyFrames < frameFrames) {
            // 装不下的部分留到下次 readData 继续，而不是丢弃
            std::lock_guard<std::mutex> lock(output_->leftoverMutex_);
            output_->leftover_.assign(frame.begin() + copySamples, frame.end());
            output_->leftoverOffset_ = 0;
        }
    }

    // 3. 仍不足时填静音补齐，并按“完整请求”返回（短读会被后端立即当作欠载
    //    再次回调，反而放大抖动）。
    size_t missingFrames = requestedFrames - totalFrames;
    if (missingFrames > 0) {
        std::memset(out + totalFrames * ch, 0,
                    missingFrames * ch * sizeof(float));
        const int n = output_->underrunCount_.fetch_add(1) + 1;
        if (n <= 1 || n % 10 == 0) {
            LOG_W("Audio underrun: %d total", n);
            emit output_->underrun();
        }
    }

    // 4. 时钟按“已交付给设备的样本”推进，静音补齐样本同样计入——
    //    时钟语义是设备播放位置（与 ffplay/SDL 回调一致），否则欠载瞬间
    //    主时钟停摆会连带视频无谓追帧。
    output_->consumedSamples_.fetch_add(requestedFrames);
    if (output_->audioClock_) {
        output_->audioClock_->addConsumedSamples(requestedFrames,
                                                  output_->outputSampleRate_);
    }

    return static_cast<qint64>(requestedFrames * ch * sizeof(float));
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

bool AudioOutput::init(int sampleRate, const AVChannelLayout* srcLayout,
                       AVSampleFormat srcFmt, AudioClock* audioClock) {
    stop();

    audioClock_ = audioClock;

    if (sampleRate <= 0) {
        LOG_E("invalid source sample rate: %d", sampleRate);
        return false;
    }

    // 保存源参数
    srcSampleRate_ = sampleRate;
    srcChannels_   = (srcLayout && srcLayout->nb_channels > 0) ? srcLayout->nb_channels : 2;
    srcFmt_        = (srcFmt == AV_SAMPLE_FMT_NONE) ? AV_SAMPLE_FMT_FLT : srcFmt;

    // 输出参数：采样率与源一致直通（最多由音频服务端重采样一次），
    // 统一为交错 float / 立体声
    outputSampleRate_ = sampleRate;
    outputChannels_   = 2;

    AVChannelLayout inLayout;
    if (srcLayout && srcLayout->nb_channels > 0) {
        av_channel_layout_copy(&inLayout, srcLayout);
    } else {
        av_channel_layout_default(&inLayout, outputChannels_);
    }
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;

    // 创建重采样上下文（源格式必须与解码器实际输出一致，如 AAC 的 FLTP；
    // 同采样率转换只做格式交织化与声道布局转换）
    int ret = swr_alloc_set_opts2(&swrCtx_,
                                  &outLayout, AV_SAMPLE_FMT_FLT, outputSampleRate_,
                                  &inLayout, srcFmt_, sampleRate,
                                  0, nullptr);
    av_channel_layout_uninit(&inLayout);
    if (ret < 0 || !swrCtx_) {
        LOG_E("swr_alloc_set_opts2 failed: %d", ret);
        swrCtx_ = nullptr;
        return false;
    }

    ret = swr_init(swrCtx_);
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
    applyVolume(); // 应用当前音量/静音状态

    initialized_ = true;
    LOG_I("AudioOutput initialized: src=%dHz %dch -> %dHz %dch FLT, fmt=%d",
          srcSampleRate_, srcChannels_, outputSampleRate_, outputChannels_,
          static_cast<int>(srcFmt_));
    return true;
}

void AudioOutput::start() {
    if (!initialized_.load()) return;
    running_ = true;
    audioIO_->open(QIODevice::ReadOnly);
    audioSink_->start(audioIO_);
    // 某些音频后端（PulseAudio/Qt6）在设备启动前调用 setVolume 可能不生效，
    // 因此设备启动后必须重设一次音量/静音状态，否则打开播放器不会有声音
    // 直到用户手动调整音量滑块（触发 setVolume → applyVolume）。
    applyVolume();
    LOG_I("AudioOutput started");
}

void AudioOutput::beginShutdown() {
    running_ = false;
    // QAudioSink::stop 同步停止设备回调，返回后不会再进入 readData
    if (audioSink_) audioSink_->stop();
    // 推进 PCM 代际并清空，唤醒任何阻塞在 writeFrames 入队循环的生产者；
    // 此刻保留 swrCtx_/audioSink_/audioIO_，供尚在运行的生产线程安全退出
    clear();
}

void AudioOutput::finishShutdown() {
    if (audioIO_) {
        audioIO_->close();
        // audioIO_ 是 QObject child（new AudioIO(this)），但每个 init()
        // 都会分配新对象覆盖原始指针，旧指针泄露。手动删除并置空。
        // QObject::~QObject() 会将其从父对象的 child 列表移除，不会二次释放。
        delete audioIO_;
        audioIO_ = nullptr;
    }

    // 提前释放 audioSink_：避免 ~AudioOutput() 中 unique_ptr 析构时 Qt6/
    // PulseAudio 内部清理触发 std::bad_array_new_length（已知 Qt6 x86-64
    // 下 PulseAudio 后端在进程退出时的偶发问题）。
    audioSink_.reset();

    if (swrCtx_) {
        swr_free(&swrCtx_);
        swrCtx_ = nullptr;
    }
    swrBuf_.clear();
    swrBuf_.shrink_to_fit();

    initialized_ = false;
    consumedSamples_ = 0;
    underrunCount_ = 0;
    LOG_I("AudioOutput stopped");
}

void AudioOutput::stop() {
    // 无线程需要 join 的场景（init 替换旧设备、析构）：一步到位
    beginShutdown();
    finishShutdown();
}

void AudioOutput::pause() {
    running_ = false;
    if (audioSink_) {
        audioSink_->suspend();
    }
}

void AudioOutput::resume() {
    if (!initialized_.load() || !audioSink_) return;
    running_ = true;
    audioSink_->resume();
}

void AudioOutput::writeFrames(const float* data, size_t frames) {
    // 注意：不能要求 running_ 才入队——首帧预滚期间音频设备尚未 start，
    // 此时解码出的 PCM 必须正常入队缓冲，否则会被整段丢弃。队列满时 push
    // 阻塞，自然形成对解码线程的背压。
    if (!initialized_.load()) return;

    size_t samples = frames * outputChannels_;
    std::vector<float> frame(data, data + samples);

    // 均衡器处理
    if (equalizer_) {
        equalizer_->process(frame.data(), frames);
    }

    // 频谱分析（EQ 之后，PCM 入队之前；交错输入直接下混，无中间分配）
    if (spectrumAnalyzer_) {
        spectrumAnalyzer_->feedInterleaved(frame.data(), frames, outputChannels_);
    }

    // 入队受阻（队列满）时的处理：
    //  - 暂停态：设备 suspend 不消费，但数据仍有效，短超时重试直到 resume，不丢帧；
    //  - seek/stop：clear() 会推进代际，在途的旧位置 PCM 必须作废，不能重新入队
    //    （否则 seek 后会先听到一小段旧声音）。
    const uint64_t gen = queueGeneration_.load();
    while (initialized_.load()) {
        if (queueGeneration_.load() != gen) {
            return; // 队列已被 seek/stop 清空，本帧作废
        }
        if (pcmQueue_.push(std::move(frame), std::chrono::milliseconds(100))) {
            if (queueGeneration_.load() == gen) {
                return; // 代际未变，稳定入队
            }
            // 与 clear() 并发：本帧可能是清空后残留的旧位置数据。
            // PCM 唯一的生产者就是本线程，此刻不可能有新代际的数据，直接清掉
            pcmQueue_.clear();
            return;
        }
    }
}

bool AudioOutput::writeFrame(AVFrame* frame) {
    std::lock_guard<std::mutex> lock(swrMutex_);
    if (!initialized_.load() || !swrCtx_) return false;

    // swr_get_out_samples 给出含内部延迟的上界，据此复用成员缓冲，
    // 不再每帧 av_samples_alloc/av_freep
    int outSamples = swr_get_out_samples(swrCtx_, frame->nb_samples);
    if (outSamples <= 0) {
        LOG_E("swr_get_out_samples returned %d", outSamples);
        return false;
    }

    swrBuf_.resize(static_cast<size_t>(outSamples) * outputChannels_);
    uint8_t* output[1] = { reinterpret_cast<uint8_t*>(swrBuf_.data()) };

    int actualSamples = swr_convert(swrCtx_, output, outSamples,
                                    (const uint8_t**)frame->data, frame->nb_samples);
    if (actualSamples < 0) {
        LOG_E("swr_convert failed: %d", actualSamples);
        return false;
    }

    if (actualSamples > 0) {
        writeFrames(swrBuf_.data(), actualSamples);
    }
    return true;
}

void AudioOutput::flushTail() {
    std::lock_guard<std::mutex> lock(swrMutex_);
    if (!initialized_.load() || !swrCtx_) return;

    // 以 NULL 输入反复调用，取出重采样器内部缓存的延迟样本
    for (;;) {
        int outSamples = swr_get_out_samples(swrCtx_, 0);
        if (outSamples <= 0) break;

        swrBuf_.resize(static_cast<size_t>(outSamples) * outputChannels_);
        uint8_t* output[1] = { reinterpret_cast<uint8_t*>(swrBuf_.data()) };

        int got = swr_convert(swrCtx_, output, outSamples, nullptr, 0);
        if (got <= 0) break;
        writeFrames(swrBuf_.data(), got);
    }
}

void AudioOutput::resetPipeline() {
    std::lock_guard<std::mutex> lock(swrMutex_);
    // swr_init 可在同一 context 上按原参数重新初始化并清空内部延迟
    // （官方文档明确支持），用于 seek 后避免输出上一段内容的重采样尾巴
    if (swrCtx_) swr_init(swrCtx_);
}

void AudioOutput::clear() {
    // 先推进代际再清空：可能正阻塞在 writeFrames 入队等待中的解码线程
    // 醒来后据此丢弃在途帧，而不是把旧位置 PCM 重新塞回空队列
    queueGeneration_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(leftoverMutex_);
        leftover_.clear();
        leftoverOffset_ = 0;
    }
    pcmQueue_.clear();
    consumedSamples_ = 0;
}

bool AudioOutput::isBufferEmpty() const {
    if (!pcmQueue_.empty()) return false;
    std::lock_guard<std::mutex> lock(leftoverMutex_);
    return leftover_.empty();
}

void AudioOutput::setVolume(int percent) {
    volumePercent_ = std::clamp(percent, 0, 100);
    applyVolume();
    LOG_I("Volume set to %d%%", volumePercent_.load());
}

void AudioOutput::setMuted(bool muted) {
    muted_ = muted;
    applyVolume();
    LOG_I("Muted: %s", muted_.load() ? "true" : "false");
}

void AudioOutput::setPlaybackSpeed(double speed) {
    speed = std::clamp(speed, 0.5, 2.0);
    speed_.store(speed);
    if (!initialized_.load()) return;

    // 有效输出率 = 源率 / speed。swr 据此转换采样数，实现变调变速。
    // QAudioSink 保持源率不变（避免跨线程重建 QObject）。
    int newOutputRate = static_cast<int>(srcSampleRate_ / speed + 0.5);
    newOutputRate = std::clamp(newOutputRate, 8000, 192000);
    if (newOutputRate == outputSampleRate_) return;

    LOG_I("setPlaybackSpeed: %.2f, srcRate=%d, swrOut=%d (QAudioSink=%d fixed)",
          speed, srcSampleRate_, newOutputRate, srcSampleRate_);

    // Step 1: 重配 swr（demux 线程执行，swrMutex_ 与音频解码线程互斥）
    {
        std::lock_guard<std::mutex> lock(swrMutex_);

        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
        AVChannelLayout inLayout;
        av_channel_layout_default(&inLayout, srcChannels_);

        SwrContext* newSwr = nullptr;
        int ret = swr_alloc_set_opts2(&newSwr,
                                       &outLayout, AV_SAMPLE_FMT_FLT, newOutputRate,
                                       &inLayout, srcFmt_, srcSampleRate_,
                                       0, nullptr);
        av_channel_layout_uninit(&inLayout);

        if (ret >= 0 && newSwr) ret = swr_init(newSwr);
        if (ret < 0 || !newSwr) {
            LOG_E("setPlaybackSpeed: swr reinit failed, keeping old config");
            swr_free(&newSwr);
            return;
        }

        swr_free(&swrCtx_);
        swrCtx_ = newSwr;
        outputSampleRate_ = newOutputRate;

        // 通知均衡器与频谱分析器新采样率，保证 IIR 系数 / FFT 频段
        // 在新速率下正确——必须在 swrMutex_ 内设置，音频解码线程持有
        // 同一锁执行 writeFrame → writeFrames → 触发 process/feed，
        // 不会拿到旧采样率处理新速率数据。
        if (equalizer_) equalizer_->setSampleRate(newOutputRate);
        if (spectrumAnalyzer_) spectrumAnalyzer_->setSampleRate(newOutputRate);
    }

    // Step 2: 立即清空旧速率 PCM（避免与 step 3 之间混合新旧数据）。
    //         队列中的数据以旧 outRate 生成，在新 outRate 下时钟计算会出错。
    {
        std::lock_guard<std::mutex> lock(leftoverMutex_);
        leftover_.clear();
        leftoverOffset_ = 0;
    }
    pcmQueue_.clear();

    // Step 3: 在主线程重启音频设备（QueuedConnection，不阻塞 demux 线程，
    //          避免 shutdown 时死锁）。此时 swr 已就绪、队列已空，重启后
    //          即开始按新速率生产/消费。
    QPointer<AudioOutput> self = this;
    QMetaObject::invokeMethod(this, [self]() {
        if (!self) return;
        if (!self->audioSink_) return; // finishShutdown 后已清理

        // 停设备 → 再清一次（step 2 到 step 3 之间入队的数据）→ 重启
        self->audioSink_->stop();
        self->audioIO_->close();

        {
            std::lock_guard<std::mutex> lock(self->leftoverMutex_);
            self->leftover_.clear();
            self->leftoverOffset_ = 0;
        }
        self->pcmQueue_.clear();

        // 重置音频时钟：consumedSamples_ → 0，pts_ → 当前位置。
        // 避免 outputSampleRate_ 变化导致 position() 跳变，
        // 进而干扰 EOF 结束判定（eofSettling 被反复复位）。
        if (self->audioClock_) {
            self->audioClock_->set(self->audioClock_->now());
        }

        if (self->running_.load()) {
            self->audioIO_->open(QIODevice::ReadOnly);
            self->audioSink_->start(self->audioIO_);
            self->applyVolume();
            LOG_I("setPlaybackSpeed: audio device restarted on main thread");
        }
    }, Qt::QueuedConnection);
}

void AudioOutput::applyVolume() {
    if (!audioSink_) return;
    // 静音时输出 0，否则按百分比线性映射到 0.0 ~ 1.0
    qreal vol = muted_.load() ? 0.0 : volumePercent_.load() / 100.0;
    audioSink_->setVolume(vol);
}
