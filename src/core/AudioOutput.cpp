#include "AudioOutput.h"
#include "MediaClock.h"
#include "Equalizer.h"
#include "SpectrumAnalyzer.h"
#include "../utils/Log.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <algorithm>
#include <cstring>

LOG_TAG("AudioOut")

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
namespace AudioNs = QtAudio;
#else
namespace AudioNs = QAudio;
#endif

namespace {
constexpr double kQueueSeconds        = 0.25;   // PCM 队列深度（变速/seek 生效延迟上限）
constexpr double kDeviceBufferSeconds = 0.10;   // 向设备申请的缓冲时长
constexpr int    kNullTickMs          = 20;
}

// ══════════════════════════════════════════════════════════════════════════
//  AudioIO
// ══════════════════════════════════════════════════════════════════════════

AudioOutput::AudioIO::AudioIO(AudioOutput* output)
    : QIODevice(output), output_(output) {}

qint64 AudioOutput::AudioIO::readData(char* data, qint64 maxSize) {
    if (!output_ || maxSize <= 0) return 0;
    const int ch = output_->channels();
    const size_t frames = static_cast<size_t>(maxSize) / (sizeof(float) * ch);
    if (frames == 0) return 0;
    output_->pullAudio(reinterpret_cast<float*>(data), frames);
    return static_cast<qint64>(frames * ch * sizeof(float));
}

qint64 AudioOutput::AudioIO::writeData(const char*, qint64) {
    return 0;
}

qint64 AudioOutput::AudioIO::bytesAvailable() const {
    if (!output_) return 0;
    // 已排队 PCM + 一秒静音余量：后端按自身缓冲空闲量决定实际读多少
    const qint64 frameBytes = static_cast<qint64>(output_->channels()) * sizeof(float);
    const qint64 queued = static_cast<qint64>(output_->queue_.queuedFrames()) * frameBytes;
    return queued + static_cast<qint64>(output_->sampleRate()) * frameBytes +
           QIODevice::bytesAvailable();
}

// ══════════════════════════════════════════════════════════════════════════
//  AudioOutput
// ══════════════════════════════════════════════════════════════════════════

AudioOutput::AudioOutput(QObject* parent)
    : QObject(parent)
{
    nullTimer_ = new QTimer(this);
    nullTimer_->setInterval(kNullTickMs);
    nullTimer_->setTimerType(Qt::PreciseTimer);
    connect(nullTimer_, &QTimer::timeout, this, &AudioOutput::nullTick);
}

AudioOutput::~AudioOutput() {
    close();
}

bool AudioOutput::open(int srcSampleRate, const AVChannelLayout* srcLayout,
                       AVSampleFormat srcFmt, AVRational timeBase, MediaClock* clock) {
    close();
    closing_ = false;
    Q_UNUSED(srcLayout);
    Q_UNUSED(srcFmt);

    if (srcSampleRate <= 0) {
        LOG_E("invalid source sample rate: %d", srcSampleRate);
        return false;
    }
    clock_ = clock;

    // 设备采样率：优先与源一致（避免双重重采样），设备不支持时退回其首选采样率
    int rate = srcSampleRate;
    QAudioFormat fmt;
    fmt.setChannelCount(channels());
    fmt.setSampleFormat(QAudioFormat::Float);
    fmt.setSampleRate(rate);
    if (!forceNull_) {
        QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (!dev.isNull() && !dev.isFormatSupported(fmt)) {
            QAudioFormat pref = dev.preferredFormat();
            int prefRate = pref.sampleRate() > 0 ? pref.sampleRate() : 48000;
            fmt.setSampleRate(prefRate);
            if (!dev.isFormatSupported(fmt)) fmt.setSampleRate(48000);
            rate = fmt.sampleRate();
            LOG_I("device does not support %d Hz, using %d Hz", srcSampleRate, rate);
        }
    }
    rate = std::clamp(rate, 8000, 192000);
    fmt.setSampleRate(rate);
    format_  = fmt;
    outRate_ = rate;

    {
        std::lock_guard<std::mutex> lock(pipeMtx_);
        pipeline_.configure(outRate_, timeBase);
        pipeline_.setTempo(speed_.load());
        serial_ = 0;
    }
    queue_.setFormat(channels(), outRate_);
    queue_.setCapacityFrames(static_cast<size_t>(outRate_ * kQueueSeconds));
    queue_.flush();

    eos_             = false;
    started_         = false;
    nullSink_        = false;
    paused_          = false;
    underrunCount_   = 0;
    pipeErrorLogged_ = false;
    latencySec_      = 0.0;
    opened_          = true;

    LOG_I("AudioOutput opened: src=%dHz -> device %dHz %dch FLT", srcSampleRate, outRate_, channels());
    return true;
}

bool AudioOutput::start() {
    if (!opened_.load()) return false;
    if (started_.load()) return true;

    bool ok = false;
    if (!forceNull_) {
        QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (dev.isNull()) {
            LOG_W("no default audio output device");
        } else {
            sink_ = std::make_unique<QAudioSink>(dev, format_, this);
            io_   = new AudioIO(this);
            io_->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
            const double bytesPerSec = static_cast<double>(outRate_) * channels() * sizeof(float);
            // 显式申请 ~100ms 设备缓冲：不指定时 PulseAudio 可能给到 2s，
            // 既放大音画误差，也让 seek 后听到大段旧位置声音
            sink_->setBufferSize(static_cast<qsizetype>(bytesPerSec * kDeviceBufferSeconds));
            sink_->start(io_);
            if (sink_->error() == AudioNs::NoError && sink_->state() != AudioNs::StoppedState) {
                ok = true;
                latencySec_ = bytesPerSec > 0 ? sink_->bufferSize() / bytesPerSec : 0.0;
                if (latencySec_ > 1.0) {
                    LOG_W("audio device buffer is unusually large (%.2f s)", latencySec_);
                    latencySec_ = 1.0;
                }
                // PCM 队列至少容纳两倍设备缓冲，设备首次填充不会把队列抽空补静音
                queue_.setCapacityFrames(static_cast<size_t>(
                    outRate_ * std::max(kQueueSeconds, 2.0 * latencySec_)));
                applyVolume();
                // 运行中设备失效（拔出、被独占）：退化为空设备，视频不停
                connect(sink_.get(), &QAudioSink::stateChanged, this, [this](AudioNs::State s) {
                    if (s == AudioNs::StoppedState && sink_ && sink_->error() != AudioNs::NoError &&
                        started_.load() && !closing_.load()) {
                        QTimer::singleShot(0, this, [this] {
                            if (sink_ && sink_->state() == AudioNs::StoppedState &&
                                sink_->error() != AudioNs::NoError && !closing_.load()) {
                                switchToNullSink("audio device stopped with error");
                            }
                        });
                    }
                });
                LOG_I("audio device started: bufferSize=%lld bytes (~%.0f ms latency)",
                      static_cast<long long>(sink_->bufferSize()), latencySec_ * 1000.0);
            } else {
                LOG_W("audio device start failed (error=%d, state=%d)",
                      static_cast<int>(sink_->error()), static_cast<int>(sink_->state()));
                sink_->stop();
                sink_.reset();
                io_->close();
                delete io_;
                io_ = nullptr;
            }
        }
    }

    if (!ok) {
        LOG_W("using null audio sink (silent, clock driven by timer)");
        nullSink_   = true;
        latencySec_ = 0.0;
        nullLast_   = std::chrono::steady_clock::now();
        nullTimer_->start();
    }
    if (latencyOverride_ >= 0.0) latencySec_ = latencyOverride_;

    started_ = true;
    paused_  = false;
    return true;
}

void AudioOutput::switchToNullSink(const char* reason) {
    LOG_W("%s: switching to null audio sink", reason);
    if (sink_) {
        sink_->stop();
        sink_.reset();
    }
    if (io_) {
        io_->close();
        delete io_;
        io_ = nullptr;
    }
    latencySec_ = latencyOverride_ >= 0.0 ? latencyOverride_ : 0.0;
    nullSink_   = true;
    nullLast_   = std::chrono::steady_clock::now();
    if (!paused_.load()) nullTimer_->start();
}

void AudioOutput::pause() {
    paused_ = true;
    if (sink_) sink_->suspend();
    if (nullSink_.load()) nullTimer_->stop();
}

void AudioOutput::resume() {
    paused_ = false;
    if (!started_.load()) return;
    if (sink_) sink_->resume();
    if (nullSink_.load()) {
        nullLast_ = std::chrono::steady_clock::now();
        if (!nullTimer_->isActive()) nullTimer_->start();
    }
}

void AudioOutput::requestStop() {
    closing_ = true;
    queue_.flush();
}

void AudioOutput::close() {
    closing_ = true;
    nullTimer_->stop();
    if (sink_) {
        sink_->stop();   // 同步停止：返回后不再进入 readData
        sink_.reset();
    }
    if (io_) {
        io_->close();
        delete io_;
        io_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(pipeMtx_);
        pipeline_.reset(-1.0);
    }
    queue_.flush();
    started_  = false;
    nullSink_ = false;
    eos_      = false;
    opened_   = false;
    clock_    = nullptr;
}

// ── 生产者 ────────────────────────────────────────────────────────────────

void AudioOutput::pushFrame(const AVFrame* frame, uint64_t serial) {
    if (!opened_.load() || closing_.load() || !frame) return;

    std::vector<PcmChunk> chunks;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(pipeMtx_);
        if (serial != serial_) return;          // seek 之前解出的旧帧
        gen = queue_.generation();
        if (!pipeline_.process(frame, chunks)) {
            if (!pipeErrorLogged_.exchange(true)) LOG_E("audio pipeline error, frame dropped");
            return;
        }
    }
    queueChunks(chunks, gen);
}

void AudioOutput::pushEndOfStream(uint64_t serial) {
    if (!opened_.load() || closing_.load()) return;

    std::vector<PcmChunk> chunks;
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(pipeMtx_);
        if (serial != serial_) return;
        gen = queue_.generation();
        pipeline_.flush(chunks);
    }
    queueChunks(chunks, gen);

    {
        std::lock_guard<std::mutex> lock(pipeMtx_);
        if (serial != serial_) return;
    }
    if (queue_.generation() != gen) return;   // 期间发生了 seek
    eos_ = true;

    // 本代际没有任何音频可交付（如 seek 到音频流结束之后），或最后一次拉取只拿到
    // 部分数据后时钟被停表：时钟不能永远停住，从当前位置按墙上时钟自走，
    // 让剩余视频继续播放、位置到达末尾
    std::lock_guard<std::mutex> lock(clockMtx_);
    if (clock_ && queue_.empty() && !clock_->isRunning() && !clock_->isPaused()) {
        clock_->update(clock_->now(), speed_.load());
    }
}

void AudioOutput::queueChunks(std::vector<PcmChunk>& chunks, uint64_t gen) {
    for (auto& c : chunks) {
        const size_t frames = c.frames(channels());
        if (frames == 0) continue;
        if (equalizer_)        equalizer_->process(c.data.data(), frames);
        if (spectrumAnalyzer_) spectrumAnalyzer_->feedInterleaved(c.data.data(), frames, channels());
        for (;;) {
            if (closing_.load() || (running_ && !running_->load())) return;
            if (queue_.push(std::move(c), gen, std::chrono::milliseconds(50))) break;
            if (queue_.generation() != gen) return;   // seek/停止：在途旧数据作废
        }
    }
}

// ── 控制 ──────────────────────────────────────────────────────────────────

void AudioOutput::flush(uint64_t serial, double targetSec) {
    {
        std::lock_guard<std::mutex> lock(pipeMtx_);
        serial_ = serial;
        pipeline_.reset(targetSec);
        eos_ = false;
    }
    std::lock_guard<std::mutex> lock(clockMtx_);
    queue_.flush();
    if (clock_ && targetSec >= 0.0) clock_->set(targetSec);
}

void AudioOutput::dropQueuedPcm() {
    std::lock_guard<std::mutex> lock(clockMtx_);
    queue_.flush();
}

void AudioOutput::setSpeed(double speed) {
    speed = std::clamp(speed, AudioPipeline::kMinTempo, AudioPipeline::kMaxTempo);
    speed_.store(speed);
    std::lock_guard<std::mutex> lock(pipeMtx_);
    pipeline_.setTempo(speed);
}

void AudioOutput::setVolume(int percent) {
    volume_ = std::clamp(percent, 0, 100);
    applyVolume();
}

void AudioOutput::setMuted(bool muted) {
    muted_ = muted;
    applyVolume();
}

void AudioOutput::applyVolume() {
    if (!sink_) return;
    qreal vol = muted_.load() ? 0.0 : volume_.load() / 100.0;
    sink_->setVolume(vol);
}

bool AudioOutput::isDrained() const {
    return eos_.load() && queue_.empty();
}

uint64_t AudioOutput::currentSerial() const {
    std::lock_guard<std::mutex> lock(pipeMtx_);
    return serial_;
}

// ── 消费端 ────────────────────────────────────────────────────────────────

size_t AudioOutput::pullAudio(float* out, size_t frames) {
    const int ch = channels();
    PcmQueue::PullResult r = queue_.pull(out, frames);
    if (r.frames < frames) {
        std::memset(out + r.frames * ch, 0, (frames - r.frames) * ch * sizeof(float));
    }

    {
        std::lock_guard<std::mutex> lock(clockMtx_);
        // 拉取期间发生了 flush：这批数据属于旧位置，时钟已由 flush 置到新目标
        if (r.generation != queue_.generation()) return frames;

        if (r.any) {
            // 时钟 = 已交付数据末尾 − 设备缓冲延迟（折算为媒体时间），且不早于本段起点
            double pos = r.endPos - latencySec_ * r.tempo;
            if (r.floorPts >= 0.0) pos = std::max(pos, r.floorPts);
            if (clock_) {
                clock_->update(pos, r.tempo);
                // 本次只拿到部分数据、其余补了静音：时钟停在真实数据末尾，
                // 静音填充期间不走时（下一块数据到达即恢复）
                if (r.frames < frames && !eos_.load()) clock_->freeze();
            }
        } else if (clock_) {
            if (eos_.load()) {
                // 尾音播完后继续走时，让位置到达末尾 / 更长的视频继续播放
                if (!clock_->isRunning() && !clock_->isPaused()) {
                    clock_->update(clock_->now(), speed_.load());
                }
            } else {
                clock_->freeze();   // 数据饥饿：停表，避免时钟跑到数据前面
            }
        }
    }

    if (r.frames < frames && !eos_.load()) {
        const int n = underrunCount_.fetch_add(1) + 1;
        if (n == 1 || n % 50 == 0) {
            LOG_W("audio underrun (%d)", n);
            emit underrun();
        }
    }
    return frames;
}

void AudioOutput::nullTick() {
    if (!nullSink_.load() || paused_.load() || closing_.load()) return;
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - nullLast_).count();
    nullLast_ = now;
    size_t frames = static_cast<size_t>(dt * outRate_);
    frames = std::min(frames, static_cast<size_t>(outRate_ / 5));   // UI 卡顿时最多补 200ms
    if (frames == 0) return;
    if (nullScratch_.size() < frames * channels()) nullScratch_.resize(frames * channels());
    pullAudio(nullScratch_.data(), frames);
}
