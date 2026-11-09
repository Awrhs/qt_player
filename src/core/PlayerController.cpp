#include "PlayerController.h"
#include "../utils/Log.h"

#include <cmath>
#include <algorithm>
#include <cstdint>

extern "C" {
#include <libavutil/version.h>
}

LOG_TAG("PlayerCtrl")

namespace {

constexpr int     kFrameQueueCapacity   = 8;
constexpr int     kPausedPrefetchFrames = 3;
constexpr double  kQueuedSecondsEnough  = 1.5;   // 每条流缓冲超过该时长即暂停读包
constexpr int     kQueuedPacketsEnough  = 100;   // 无时长信息时按包数
constexpr int64_t kQueuedBytesMax       = 16LL * 1024 * 1024;
constexpr double  kEofSettleSec         = 0.20;  // 位置到达末尾后的稳态等待
constexpr double  kEofMaxWaitSec        = 1.50;  // 数据排空后最多等待位置到末尾的时间
constexpr double  kAudioStartTimeoutSec = 2.0;
constexpr double  kClockStallTimeoutSec = 3.0;
constexpr double  kSpeedMin             = 0.5;
constexpr double  kSpeedMax             = 2.0;

using SteadyClock = std::chrono::steady_clock;

double ptsToSec(const AVFrame* f, AVRational tb, double fallback) {
    int64_t pts = f->pts;
    if (pts == AV_NOPTS_VALUE) pts = f->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE || tb.num <= 0 || tb.den <= 0) return fallback;
    return pts * av_q2d(tb);
}

double frameDurSec(const AVFrame* f, AVRational tb, double fallback) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 30, 100)
    const int64_t d = f->duration;
#else
    const int64_t d = f->pkt_duration;
#endif
    if (d > 0 && tb.num > 0 && tb.den > 0) return d * av_q2d(tb);
    return fallback;
}

double secondsSince(SteadyClock::time_point tp) {
    return std::chrono::duration<double>(SteadyClock::now() - tp).count();
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════
//  构造 / 析构
// ══════════════════════════════════════════════════════════════════════════

PlayerController::PlayerController(QObject* parent)
    : QObject(parent),
      frameQueue_(kFrameQueueCapacity)
{
    equalizer_        = std::make_unique<Equalizer>();
    spectrumAnalyzer_ = std::make_unique<SpectrumAnalyzer>();

    renderTimer_ = new QTimer(this);
    renderTimer_->setSingleShot(true);
    renderTimer_->setTimerType(Qt::PreciseTimer);
    connect(renderTimer_, &QTimer::timeout, this, &PlayerController::onRenderTick);
}

PlayerController::~PlayerController() {
    closeMedia();
}

// ══════════════════════════════════════════════════════════════════════════
//  Public API
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::open(const QString& path) {
    open(path, hwDecodeEnabled_);
}

void PlayerController::open(const QString& path, bool useHardwareDecode) {
    LOG_I("open: %s (hw=%d)", path.toUtf8().constData(), useHardwareDecode);
    hwDecodeEnabled_ = useHardwareDecode;

    closeMedia();
    path_ = path;

    // 单媒体状态复位
    ended_              = false;
    firstFrameShown_    = false;
    audioStartPending_  = false;
    eofSettling_        = false;
    drainedSince_       = {};
    stepped_            = false;
    showFrameWhenPaused_ = false;
    lastFrameStreamPts_ = 0.0;
    lastFrameDur_       = 0.0;
    lastFrameUiPts_     = 0.0;
    droppedTotal_       = 0;
    statDropped_        = 0;
    loopSeekSerial_     = 0;
    currentAspect_      = -1.0;
    emit displayAspectChanged(-1.0);

    demuxer_ = std::make_unique<Demuxer>();
    abortIo_ = false;
    demuxer_->setAbortFlag(&abortIo_);
    if (!demuxer_->open(path.toStdString())) {
        demuxer_.reset();
        state_ = PlayState::Idle;
        emit errorOccurred(QStringLiteral("无法打开文件: %1").arg(path));
        emit stateChanged(PlayState::Idle);
        return;
    }

    AVFormatContext* fmt = demuxer_->formatCtx();
    hasVideo_   = demuxer_->videoIndex() >= 0;
    hasAudio_   = demuxer_->audioIndex() >= 0;
    startTime_  = demuxer_->startTimeSec();
    duration_   = demuxer_->durationSec();
    videoTb_    = demuxer_->videoTimeBase();
    audioTb_    = demuxer_->audioTimeBase();
    const double fps = demuxer_->videoFrameRate();
    defaultFrameDur_ = fps > 0.0 ? 1.0 / fps : 0.04;

    // 视频解码器 + 渲染器
    if (hasVideo_) {
        AVCodecParameters* par = fmt->streams[demuxer_->videoIndex()]->codecpar;
        videoDecoder_ = std::make_unique<Decoder>();
        bool ok = videoDecoder_->open(par, videoTb_, hwDecodeEnabled_);
        if (!ok && hwDecodeEnabled_) {
            LOG_I("Retrying video decoder with software decoding");
            hwDecodeEnabled_ = false;
            ok = videoDecoder_->open(par, videoTb_, false);
        }
        if (!ok) {
            videoDecoder_.reset();
            if (!hasAudio_) {
                demuxer_.reset();
                state_ = PlayState::Idle;
                emit errorOccurred(QStringLiteral("无法初始化视频解码器"));
                emit stateChanged(PlayState::Idle);
                return;
            }
            LOG_W("Video decoder unavailable, playing audio only");
            emit warning(QStringLiteral("视频解码器不可用，仅播放音频"));
            hasVideo_ = false;
        } else {
            videoRenderer_ = std::make_unique<VideoRenderer>();
            if (!videoRenderer_->open(par->width, par->height,
                                      static_cast<AVPixelFormat>(par->format))) {
                // 像素格式可能要到首帧才确定，渲染器会在首帧按实际参数重建
                LOG_W("Video renderer deferred to first frame (w=%d h=%d fmt=%d)",
                      par->width, par->height, par->format);
            }
        }
    }

    // 音频解码器 + 音频输出
    if (hasAudio_) {
        AVCodecParameters* par = fmt->streams[demuxer_->audioIndex()]->codecpar;
        audioDecoder_ = std::make_unique<Decoder>();
        if (!audioDecoder_->open(par, audioTb_, false)) {
            LOG_W("Failed to open audio decoder, continuing without audio");
            audioDecoder_.reset();
            hasAudio_ = false;
        } else {
            audioOutput_ = std::make_unique<AudioOutput>();
            audioOutput_->setStopToken(&running_);
            if (!audioOutput_->open(par->sample_rate, &par->ch_layout,
                                    static_cast<AVSampleFormat>(par->format),
                                    audioTb_, &clock_)) {
                LOG_W("Failed to init audio output, continuing without audio");
                audioOutput_.reset();
                audioDecoder_.reset();
                hasAudio_ = false;
            } else {
                audioOutput_->setEqualizer(equalizer_.get());
                audioOutput_->setSpectrumAnalyzer(spectrumAnalyzer_.get());
                equalizer_->setSampleRate(audioOutput_->sampleRate());
                spectrumAnalyzer_->setSampleRate(audioOutput_->sampleRate());
                audioOutput_->setVolume(volume_);
                audioOutput_->setMuted(muted_);
                audioOutput_->setSpeed(speed_.load());
            }
        }
    }

    if (!hasVideo_ && !hasAudio_) {
        closeMedia();
        state_ = PlayState::Idle;
        emit errorOccurred(QStringLiteral("文件中没有可播放的流"));
        emit stateChanged(PlayState::Idle);
        return;
    }

    // 解码器不可用的流让解复用器直接丢弃
    if (!hasVideo_ && demuxer_->videoIndex() >= 0)
        fmt->streams[demuxer_->videoIndex()]->discard = AVDISCARD_ALL;
    if (!hasAudio_ && demuxer_->audioIndex() >= 0)
        fmt->streams[demuxer_->audioIndex()]->discard = AVDISCARD_ALL;

    // 新文件：清掉上一曲目的 EQ IIR 残留与频谱累积（工作线程启动前，无竞争）
    equalizer_->clear();
    spectrumAnalyzer_->reset();

    serial_         = 1;
    audioEosSerial_ = 0;
    videoEosSerial_ = 0;
    demuxEof_       = false;
    seekPendingFlag_ = false;
    audioPackets_.flush(1, -1.0);
    videoPackets_.flush(1, -1.0);

    clock_.resume();
    clock_.setSpeed(speed_.load());
    clock_.set(startTime_);
    if (audioOutput_) audioOutput_->flush(1, startTime_);
    clockStallSince_ = {};

    state_ = PlayState::Playing;
    startThreads();

    if (hasAudio_) {
        if (hasVideo_) {
            // 音视频预滚：首帧就绪再启动音频设备，避免视频解码预热期间音频先行
            audioStartPending_ = true;
        } else {
            audioOutput_->start();
        }
    }
    playStartTp_ = SteadyClock::now();
    lastStatTp_  = playStartTp_;

    LOG_I("opened: video=%d audio=%d start=%.3f duration=%.3f fps=%.3f",
          hasVideo_, hasAudio_, startTime_, duration_, fps);

    emit durationChanged(duration_);
    emit mediaOpened(path, hasVideo_, hasAudio_);
    emit stateChanged(PlayState::Playing);
    scheduleRender(0);
}

void PlayerController::play() {
    if (state_.load() != PlayState::Paused) return;

    const bool restart = ended_;
    const bool realign = stepped_ && hasAudio_;
    ended_   = false;
    stepped_ = false;
    showFrameWhenPaused_ = false;
    // 先置 Playing 再请求 seek：processSeek 不会把它当作暂停态 seek
    state_ = PlayState::Playing;

    if (restart || realign) {
        // 暂停点残留的 PCM 不该在 seek 生效前被播出
        if (audioOutput_) audioOutput_->dropQueuedPcm();
        if (restart) {
            LOG_I("Play pressed at end, restarting from beginning");
            requestSeek(startTime_);
        } else {
            // 逐帧后画面已前进而音频仍停在暂停点：精确 seek 到画面位置让音频对齐
            requestSeek(lastFrameStreamPts_);
        }
    }
    resumePlayback();
}

void PlayerController::resumePlayback() {
    state_ = PlayState::Playing;
    eofSettling_ = false;
    drainedSince_ = {};
    clock_.resume();
    if (audioOutput_) {
        if (audioStartPending_) {
            if (firstFrameShown_ || !hasVideo_) {
                audioOutput_->start();
                audioStartPending_ = false;
            }
        } else {
            audioOutput_->resume();
        }
    }
    playStartTp_ = SteadyClock::now();
    emit stateChanged(PlayState::Playing);
    scheduleRender(0);
}

void PlayerController::pause() {
    if (state_.load() != PlayState::Playing) return;
    state_ = PlayState::Paused;
    showFrameWhenPaused_.store(false);
    clockStallSince_ = {};
    clock_.pause();
    if (audioOutput_) audioOutput_->pause();
    emit stateChanged(PlayState::Paused);
    emit positionChanged(position());
}

void PlayerController::togglePlayPause() {
    if (state_.load() == PlayState::Playing) pause();
    else if (state_.load() == PlayState::Paused) play();
}

void PlayerController::stop() {
    if (state_.load() == PlayState::Idle || state_.load() == PlayState::Stopped) return;
    LOG_I("stop");
    closeMedia();
    ended_    = false;
    duration_ = 0.0;
    state_    = PlayState::Stopped;
    emit stateChanged(PlayState::Stopped);
}

void PlayerController::shutdown() {
    LOG_I("shutdown: synchronous stop before close");
    closeMedia();
    state_ = PlayState::Stopped;
}

void PlayerController::seek(double seconds) {
    const PlayState st = state_.load();
    if (!demuxer_ || st == PlayState::Idle || st == PlayState::Stopped) return;
    double ui = seconds;
    if (duration_ > 0.0) ui = std::clamp(ui, 0.0, duration_);
    else ui = std::max(0.0, ui);
    LOG_I("seek requested: %.3f (state=%d)", ui, static_cast<int>(st));
    ended_   = false;
    stepped_ = false;
    requestSeek(startTime_ + ui);
}

void PlayerController::seekRelative(double deltaSeconds) {
    seek(position() + deltaSeconds);
}

void PlayerController::setSpeed(double factor) {
    const double s = std::clamp(factor, kSpeedMin, kSpeedMax);
    speed_.store(s);
    if (audioOutput_) audioOutput_->setSpeed(s);
    clock_.setSpeed(s);
    LOG_I("Playback speed set to %.2f", s);
    emit speedChanged(s);
}

void PlayerController::setVolume(int percent) {
    volume_ = std::clamp(percent, 0, 100);
    if (audioOutput_) audioOutput_->setVolume(volume_);
}

void PlayerController::setMuted(bool muted) {
    muted_ = muted;
    if (audioOutput_) audioOutput_->setMuted(muted);
}

void PlayerController::setLoop(bool enabled) {
    if (loop_.exchange(enabled) == enabled) return;
    LOG_I("loop %s", enabled ? "on" : "off");
    emit loopChanged(enabled);
}

void PlayerController::setVideoTargetSize(int w, int h) {
    if (videoRenderer_) videoRenderer_->setTargetSize(w, h);
}

void PlayerController::stepForward() {
    if (!demuxer_ || !hasVideo_) return;
    if (state_.load() == PlayState::Playing) pause();
    if (state_.load() != PlayState::Paused || ended_) return;

    VideoFrame vf;
    if (pendingFrame_.frame) {
        if (pendingFrame_.serial == serial_.load()) {
            vf = pendingFrame_;
            pendingFrame_ = {};
        } else {
            releaseFrame(pendingFrame_);
        }
    }
    if (!vf.frame) {
        const bool drained = demuxEof_.load() && videoEosSerial_.load() == serial_.load() &&
                             frameQueue_.empty();
        if (drained || !popFrame(vf, 300)) {
            LOG_W("stepForward: no frame available");
            return;
        }
    }
    displayFrame(vf);
    clock_.set(lastFrameStreamPts_);
    stepped_ = true;
    showFrameWhenPaused_ = false;
    emit positionChanged(position());
}

void PlayerController::stepBackward() {
    if (!demuxer_ || !hasVideo_) return;
    if (state_.load() == PlayState::Playing) pause();
    if (state_.load() != PlayState::Paused) return;

    const double dur = lastFrameDur_ > 0.0 ? lastFrameDur_ : defaultFrameDur_;
    const double target = std::max(startTime_, lastFrameStreamPts_ - 0.5 * dur);
    LOG_I("stepBackward: current frame %.3f -> target %.3f", lastFrameStreamPts_, target);
    ended_   = false;
    stepped_ = false;
    requestSeek(target);
}

double PlayerController::position() const {
    if (seekPendingFlag_.load()) return pendingSeekUi_.load();
    double p = clock_.now() - startTime_;
    if (duration_ > 0.0) p = std::min(p, duration_);
    return std::max(0.0, p);
}

int PlayerController::videoWidth() const {
    if (!demuxer_ || demuxer_->videoIndex() < 0) return 0;
    return demuxer_->formatCtx()->streams[demuxer_->videoIndex()]->codecpar->width;
}

int PlayerController::videoHeight() const {
    if (!demuxer_ || demuxer_->videoIndex() < 0) return 0;
    return demuxer_->formatCtx()->streams[demuxer_->videoIndex()]->codecpar->height;
}

bool PlayerController::audioUsingNullSink() const {
    return audioOutput_ && audioOutput_->usingNullSink();
}

// ══════════════════════════════════════════════════════════════════════════
//  seek 请求
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::requestSeek(double targetStreamSec) {
    {
        std::lock_guard<std::mutex> lock(seekMtx_);
        seekTarget_  = targetStreamSec;
        seekPending_ = true;
    }
    pendingSeekUi_.store(std::max(0.0, targetStreamSec - startTime_));
    seekPendingFlag_.store(true);
    seekCv_.notify_all();
    emit positionChanged(pendingSeekUi_.load());
}

bool PlayerController::takePendingSeek(double& target) {
    std::lock_guard<std::mutex> lock(seekMtx_);
    if (!seekPending_) return false;
    target       = seekTarget_;
    seekPending_ = false;
    return true;
}

void PlayerController::waitIdle(int ms) {
    std::unique_lock<std::mutex> lock(seekMtx_);
    seekCv_.wait_for(lock, std::chrono::milliseconds(ms),
                     [this] { return seekPending_ || !running_.load(); });
}

void PlayerController::processSeek(double target) {
    const double lo = startTime_;
    const double hi = duration_ > 0.0 ? startTime_ + duration_ : std::max(lo, target);
    target = std::clamp(target, lo, std::max(lo, hi));
    LOG_I("seek -> %.3f (stream), %.3f (ui)", target, target - startTime_);

    if (!demuxer_->seek(target)) {
        seekPendingFlag_.store(false);
        emit warning(QStringLiteral("无法定位到该位置"));
        return;
    }

    const uint64_t s = serial_.fetch_add(1) + 1;
    // 先切换音频输出代际，再清包队列：避免新代际首帧被旧代际检查误丢。
    // 有音频时时钟由 AudioOutput::flush 在其时钟锁内置位，与设备回调的锚定串行化
    if (audioOutput_) audioOutput_->flush(s, target);
    else clock_.set(target);
    audioPackets_.flush(s, target);
    videoPackets_.flush(s, target);
    clearFrameQueue();
    demuxEof_.store(false);
    if (state_.load() == PlayState::Paused) showFrameWhenPaused_.store(true);
    seekPendingFlag_.store(false);
    emit positionChanged(std::max(0.0, target - startTime_));
}

bool PlayerController::queuesHaveEnough() const {
    auto enough = [](const PacketQueue& q, AVRational tb) {
        const int n = q.nbPackets();
        if (n <= 0) return false;
        const int64_t durTb = q.durationTb();
        if (durTb > 0 && tb.num > 0 && tb.den > 0) {
            return durTb * av_q2d(tb) > kQueuedSecondsEnough || n > kQueuedPacketsEnough * 5;
        }
        return n > kQueuedPacketsEnough;
    };
    const bool audioEnough = !hasAudio_ || enough(audioPackets_, audioTb_);
    const bool videoEnough = !hasVideo_ || enough(videoPackets_, videoTb_);
    const int64_t total = audioPackets_.sizeBytes() + videoPackets_.sizeBytes();
    return total > kQueuedBytesMax || (audioEnough && videoEnough);
}

// ══════════════════════════════════════════════════════════════════════════
//  渲染 / 同步（主线程）
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::scheduleRender(int ms) {
    renderTimer_->start(std::max(0, ms));
}

double PlayerController::frameTime(const AVFrame* f) const {
    return ptsToSec(f, videoTb_, lastFrameStreamPts_ + lastFrameDur_);
}

bool PlayerController::audioDrivesClock() const {
    return hasAudio_ && audioOutput_ != nullptr;
}

void PlayerController::releaseFrame(VideoFrame& vf) {
    if (vf.frame) av_frame_free(&vf.frame);
    vf = {};
}

void PlayerController::clearFrameQueue() {
    VideoFrame vf;
    while (frameQueue_.pop(vf, std::chrono::milliseconds(0))) {
        releaseFrame(vf);
    }
}

bool PlayerController::popFrame(VideoFrame& out, int waitMs) {
    const auto deadline = SteadyClock::now() + std::chrono::milliseconds(waitMs);
    for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - SteadyClock::now());
        if (remaining.count() < 0) remaining = std::chrono::milliseconds(0);
        if (!frameQueue_.pop(out, remaining)) return false;
        if (out.serial == serial_.load()) return true;
        releaseFrame(out);   // seek 之前解出的旧帧
        if (SteadyClock::now() >= deadline) return false;
    }
}

void PlayerController::displayFrame(VideoFrame& vf) {
    AVFrame* f = vf.frame;
    vf = {};
    if (!f) return;

    const double pts = frameTime(f);
    const double dur = frameDurSec(f, videoTb_, defaultFrameDur_);

    updateDisplayAspect(f);
    QImage img = videoRenderer_ ? videoRenderer_->render(f) : QImage();
    av_frame_free(&f);

    lastFrameStreamPts_ = pts;
    lastFrameDur_       = dur;
    lastFrameUiPts_     = std::max(0.0, pts - startTime_);

    if (!img.isNull()) {
        if (!firstFrameShown_) {
            firstFrameShown_ = true;
            emit firstFrameRendered();
        }
        emit frameReady(img);
    }
    emit frameDisplayed(lastFrameUiPts_);
}

void PlayerController::onRenderTick() {
    const PlayState st = state_.load();
    if (st == PlayState::Idle || st == PlayState::Stopped) return;

    const auto now = SteadyClock::now();
    if (now - lastStatTp_ >= std::chrono::seconds(1)) {
        if (statDropped_ > 0) LOG_W("Dropped %d late frame(s) in the last second", statDropped_);
        statDropped_ = 0;
        lastStatTp_  = now;
    }

    // 兜底：视频迟迟出不了首帧时，不能让音频永远挂起不启动
    if (audioStartPending_ && st == PlayState::Playing && audioOutput_ &&
        secondsSince(playStartTp_) > kAudioStartTimeoutSec) {
        LOG_W("First video frame timeout, starting audio anyway");
        audioOutput_->start();
        audioStartPending_ = false;
    }

    if (checkEndOfStream()) {
        scheduleRender(50);
        return;
    }

    if (st == PlayState::Paused) {
        // 暂停态 seek / 逐帧后退：显示一帧目标位置的画面但保持暂停
        if (showFrameWhenPaused_.load()) {
            VideoFrame vf;
            if (popFrame(vf, 0)) {
                displayFrame(vf);
                showFrameWhenPaused_.store(false);
                emit positionChanged(position());
            }
        }
        scheduleRender(30);
        return;
    }

    if (!hasVideo_) {
        scheduleRender(50);
        return;
    }

    VideoFrame vf;
    if (pendingFrame_.frame) {
        if (pendingFrame_.serial != serial_.load()) {
            releaseFrame(pendingFrame_);
        } else {
            vf = pendingFrame_;
            pendingFrame_ = {};
        }
    }
    if (!vf.frame && !popFrame(vf, 0)) {
        scheduleRender(5);
        return;
    }

    // 首帧已就绪：此时再启动音频设备，音频时钟不会在视频预热期间先行
    if (audioStartPending_ && audioOutput_) {
        audioOutput_->start();
        audioStartPending_ = false;
    }

    // 视频-only：首帧（或 seek 后首帧）锚定时钟
    if (clock_.isWaiting() && !audioDrivesClock()) {
        clock_.update(frameTime(vf.frame), speed_.load());
        if (vf.serial != serial_.load()) {
            // 锚定瞬间发生了 seek：撤销，时钟回到新目标点等待
            clock_.set(videoPackets_.seekTarget());
            releaseFrame(vf);
            scheduleRender(5);
            return;
        }
    }

    // 看门狗：音频应驱动时钟却长时间不走（解码器无输出、滤镜失败、设备异常），
    // 改为按墙上时钟自走，视频不能因此永远停住
    if (audioDrivesClock() && !clock_.isRunning() && !clock_.isPaused()) {
        if (clockStallSince_ == SteadyClock::time_point{}) {
            clockStallSince_ = now;
        } else if (secondsSince(clockStallSince_) > kClockStallTimeoutSec) {
            LOG_W("Audio clock stalled for %.1fs, running on wall clock", kClockStallTimeoutSec);
            clock_.update(clock_.isWaiting() ? frameTime(vf.frame) : clock_.now(), speed_.load());
            clockStallSince_ = {};
        }
    } else {
        clockStallSince_ = {};
    }

    double delay = frameTime(vf.frame) - clock_.now();
    if (delay > 0.005) {
        pendingFrame_ = vf;
        scheduleRender(std::clamp(static_cast<int>(delay * 1000.0), 1, 100));
        return;
    }

    // 已到期：若后继帧也已到期，则当前帧过时，丢弃以追赶
    for (;;) {
        VideoFrame next;
        if (!popFrame(next, 0)) break;
        const double nd = frameTime(next.frame) - clock_.now();
        if (nd <= 0.0) {
            releaseFrame(vf);
            vf = next;
            statDropped_++;
            droppedTotal_++;
            continue;
        }
        pendingFrame_ = next;
        break;
    }

    displayFrame(vf);

    if (pendingFrame_.frame) {
        const double d = frameTime(pendingFrame_.frame) - clock_.now();
        scheduleRender(std::clamp(static_cast<int>(d * 1000.0), 1, 100));
    } else {
        scheduleRender(2);
    }
}

bool PlayerController::checkEndOfStream() {
    if (ended_) return true;
    if (!demuxEof_.load() || seekPendingFlag_.load()) {
        eofSettling_  = false;
        drainedSince_ = {};
        return false;
    }

    const uint64_t s = serial_.load();
    if (hasAudio_ && audioOutput_) {
        if (audioEosSerial_.load() != s || !audioOutput_->isDrained()) {
            eofSettling_  = false;
            drainedSince_ = {};
            return false;
        }
    }
    if (hasVideo_) {
        if (videoEosSerial_.load() != s || !frameQueue_.empty() || pendingFrame_.frame) {
            eofSettling_  = false;
            drainedSince_ = {};
            return false;
        }
    }
    if (state_.load() != PlayState::Playing) return false;

    const auto now = SteadyClock::now();
    if (drainedSince_ == SteadyClock::time_point{}) drainedSince_ = now;

    if (loop_.load()) {
        if (loopSeekSerial_ != s) {
            loopSeekSerial_ = s;
            LOG_I("End of stream reached, looping to start");
            requestSeek(startTime_);
        }
        return false;
    }

    // 设备缓冲中还有尾音：等位置到达末尾 + 稳态，但不无限等待
    const double endPos = startTime_ + duration_;
    const double pos    = clock_.now();
    const double waited = std::chrono::duration<double>(now - drainedSince_).count();
    if (duration_ > 0.0 && pos < endPos - 0.05 && waited < kEofMaxWaitSec) {
        eofSettling_ = false;
        return false;
    }
    if (!eofSettling_) {
        eofSettling_  = true;
        eofSettleTp_  = now;
        return false;
    }
    if (std::chrono::duration<double>(now - eofSettleTp_).count() < kEofSettleSec) return false;

    enterPlaybackFinished();
    return true;
}

void PlayerController::enterPlaybackFinished() {
    LOG_I("Playback finished, entering end-paused mode");
    ended_        = true;
    eofSettling_  = false;
    drainedSince_ = {};
    state_        = PlayState::Paused;

    clock_.pause();
    if (duration_ > 0.0) clock_.set(startTime_ + duration_);
    if (audioOutput_) audioOutput_->pause();

    emit stateChanged(PlayState::Paused);
    emit positionChanged(position());
    emit playbackFinished();
}

void PlayerController::updateDisplayAspect(AVFrame* frame) {
    if (!frame || frame->height <= 0) return;
    double sar = 1.0;
    if (frame->sample_aspect_ratio.num > 0 &&
        frame->sample_aspect_ratio.den > 0) {
        sar = av_q2d(frame->sample_aspect_ratio);
    }
    double aspect = static_cast<double>(frame->width) * sar / frame->height;
    if (aspect <= 0.0) return;
    // 变化超过 0.5% 才通知，避免逐帧浮点抖动重建 sws
    if (currentAspect_ < 0.0 ||
        std::fabs(aspect - currentAspect_) / currentAspect_ > 0.005) {
        currentAspect_ = aspect;
        emit displayAspectChanged(aspect);
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  解复用线程
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::demuxLoop() {
    LOG_I("DemuxThread started");
    const int aIdx = demuxer_->audioIndex();
    const int vIdx = demuxer_->videoIndex();

    while (running_.load()) {
        double target = 0.0;
        if (takePendingSeek(target)) {
            processSeek(target);
            continue;
        }

        if (demuxEof_.load() || queuesHaveEnough()) {
            waitIdle(10);
            continue;
        }

        AVPacket* pkt = nullptr;
        const ReadStatus st = demuxer_->readPacket(pkt);
        if (st == ReadStatus::Again) {
            waitIdle(10);
            continue;
        }
        if (st != ReadStatus::Ok) {
            if (!running_.load()) break;
            LOG_I("Demux %s reached", st == ReadStatus::Eof ? "EOF" : "error");
            // 读错误同样进入 drain：已缓冲的数据仍可播完
            if (hasAudio_) audioPackets_.put(nullptr);
            if (hasVideo_) videoPackets_.put(nullptr);
            demuxEof_.store(true);
            continue;
        }

        if (hasAudio_ && pkt->stream_index == aIdx) {
            audioPackets_.put(pkt);
        } else if (hasVideo_ && pkt->stream_index == vIdx) {
            videoPackets_.put(pkt);
        } else {
            av_packet_free(&pkt);
        }
    }
    LOG_I("DemuxThread stopped");
}

// ══════════════════════════════════════════════════════════════════════════
//  音频解码线程
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::audioDecodeLoop() {
    LOG_I("AudioDecodeThread started");
    uint64_t decSerial = UINT64_MAX;
    AVFrame* frame = av_frame_alloc();
    bool errLogged = false;

    auto receiveAll = [&]() {
        while (running_.load()) {
            int ret = audioDecoder_->receiveFrame(frame);
            if (ret != 0) break;
            audioOutput_->pushFrame(frame, decSerial);
            av_frame_unref(frame);
        }
    };

    while (running_.load() && frame) {
        PacketItem item;
        if (!audioPackets_.get(item, std::chrono::milliseconds(50))) continue;

        if (item.serial != decSerial) {
            audioDecoder_->flush();
            decSerial = item.serial;
        }

        if (!item.pkt) {
            // EOF 哨兵：排空解码器 → 排空滤镜尾部 → 标记本代际音频结束
            audioDecoder_->sendPacket(nullptr);
            receiveAll();
            audioDecoder_->flush();
            audioOutput_->pushEndOfStream(decSerial);
            audioEosSerial_.store(decSerial);
            LOG_I("Audio decoder drained to EOF (serial %llu)",
                  static_cast<unsigned long long>(decSerial));
            continue;
        }

        for (int attempt = 0; attempt < 3; ++attempt) {
            int ret = audioDecoder_->sendPacket(item.pkt);
            if (ret == AVERROR(EAGAIN)) {
                receiveAll();
                continue;
            }
            if (ret < 0 && !errLogged) {
                LOG_W("Audio sendPacket error: %d", ret);
                errLogged = true;
            }
            break;
        }
        av_packet_free(&item.pkt);
        receiveAll();
    }

    av_frame_free(&frame);
    LOG_I("AudioDecodeThread stopped");
}

// ══════════════════════════════════════════════════════════════════════════
//  视频解码线程
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::videoDecodeLoop() {
    LOG_I("VideoDecodeThread started");
    uint64_t decSerial    = UINT64_MAX;
    double   discardUntil = -1.0;
    double   lastPts      = startTime_;
    const AVRational tb   = videoTb_;
    const double defDur   = defaultFrameDur_;
    bool errLogged = false;

    // 帧队列持有独立的 AVFrame*（主线程逐个 av_frame_free）
    auto handleFrame = [&](AVFrame*& f) {
        const double pts = ptsToSec(f, tb, lastPts + defDur);
        const double dur = frameDurSec(f, tb, defDur);
        lastPts = pts;
        if (discardUntil >= 0.0) {
            // seek pre-roll：整帧结束于目标点之前 → 丢弃；包含目标点的帧开始上屏
            if (pts + dur <= discardUntil + 1e-6) {
                av_frame_free(&f);
                return;
            }
            discardUntil = -1.0;
        }
        VideoFrame vf;
        vf.frame  = f;
        vf.serial = decSerial;
        f = nullptr;
        while (running_.load()) {
            if (frameQueue_.push(vf, std::chrono::milliseconds(50))) {
                vf.frame = nullptr;
                break;
            }
            if (videoPackets_.serial() != decSerial) break;   // 期间 seek 了：丢弃
        }
        if (vf.frame) av_frame_free(&vf.frame);
    };

    auto receiveAll = [&]() {
        while (running_.load()) {
            AVFrame* f = av_frame_alloc();
            if (!f) return;
            int ret = videoDecoder_->receiveFrame(f);
            if (ret != 0) {
                av_frame_free(&f);
                return;
            }
            handleFrame(f);
        }
    };

    while (running_.load()) {
        // 暂停态只预解码少量帧供逐帧使用
        if (state_.load() == PlayState::Paused &&
            frameQueue_.size() >= static_cast<size_t>(kPausedPrefetchFrames)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        PacketItem item;
        if (!videoPackets_.get(item, std::chrono::milliseconds(50))) continue;

        if (item.serial != decSerial) {
            videoDecoder_->flush();
            decSerial    = item.serial;
            discardUntil = videoPackets_.seekTarget();
            if (discardUntil >= 0.0) lastPts = discardUntil;
        }

        if (!item.pkt) {
            videoDecoder_->sendPacket(nullptr);
            receiveAll();
            videoDecoder_->flush();
            videoEosSerial_.store(decSerial);
            LOG_I("Video decoder drained to EOF (serial %llu)",
                  static_cast<unsigned long long>(decSerial));
            continue;
        }

        for (int attempt = 0; attempt < 3; ++attempt) {
            int ret = videoDecoder_->sendPacket(item.pkt);
            if (ret == AVERROR(EAGAIN)) {
                receiveAll();
                continue;
            }
            if (ret < 0 && !errLogged) {
                LOG_W("Video sendPacket error: %d", ret);
                errLogged = true;
            }
            break;
        }
        av_packet_free(&item.pkt);
        receiveAll();
    }

    LOG_I("VideoDecodeThread stopped");
}

// ══════════════════════════════════════════════════════════════════════════
//  线程管理
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::startThreads() {
    running_ = true;
    demuxThread_ = std::thread(&PlayerController::demuxLoop, this);
    if (hasAudio_) audioDecodeThread_ = std::thread(&PlayerController::audioDecodeLoop, this);
    if (hasVideo_) videoDecodeThread_ = std::thread(&PlayerController::videoDecodeLoop, this);
    LOG_I("Threads started (demux%s%s)", hasAudio_ ? " + audio" : "", hasVideo_ ? " + video" : "");
}

void PlayerController::closeMedia() {
    running_ = false;
    abortIo_ = true;
    seekCv_.notify_all();
    if (audioOutput_) audioOutput_->requestStop();
    clearFrameQueue();   // 唤醒阻塞在入队上的视频解码线程

    if (demuxThread_.joinable())       demuxThread_.join();
    if (audioDecodeThread_.joinable()) audioDecodeThread_.join();
    if (videoDecodeThread_.joinable()) videoDecodeThread_.join();

    renderTimer_->stop();
    clearFrameQueue();
    releaseFrame(pendingFrame_);
    audioPackets_.clear();
    videoPackets_.clear();

    if (audioOutput_) {
        audioOutput_->close();
        audioOutput_.reset();
    }
    videoDecoder_.reset();
    audioDecoder_.reset();
    videoRenderer_.reset();
    demuxer_.reset();

    hasVideo_ = false;
    hasAudio_ = false;
    demuxEof_.store(false);
    seekPendingFlag_.store(false);
    {
        std::lock_guard<std::mutex> lock(seekMtx_);
        seekPending_ = false;
    }
    showFrameWhenPaused_.store(false);
    audioStartPending_ = false;
}
