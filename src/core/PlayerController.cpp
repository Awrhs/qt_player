#include "PlayerController.h"
#include "../utils/Log.h"

#include <QPointer>
#include <cmath>
#include <algorithm>

LOG_TAG("PlayerCtrl")

PlayerController::PlayerController(QObject* parent)
    : QObject(parent)
{
    equalizer_       = std::make_unique<Equalizer>();
    spectrumAnalyzer_ = std::make_unique<SpectrumAnalyzer>();

    renderTimer_ = new QTimer(this);
    connect(renderTimer_, &QTimer::timeout, this, &PlayerController::onRenderTick);
}

PlayerController::~PlayerController() {
    stopThreads();
}

// ══════════════════════════════════════════════════════════════════════════
//  Public API
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::open(const QString& path) {
    open(path, false);
}

void PlayerController::open(const QString& path, bool useHardwareDecode) {
    LOG_I("open: %s (hw=%d)", path.toUtf8().constData(), useHardwareDecode);

    hwDecodeEnabled_ = useHardwareDecode;

    stopThreads();
    renderTimer_->stop();

    frameCount_ = 0;
    audioStartPending_ = false;
    clockAnchorPending_ = false;
    resetEofState();
    currentAspect_ = -1.0;
    emit displayAspectChanged(-1.0);

    // 重置模块
    demuxer_       = std::make_unique<Demuxer>();
    videoDecoder_  = std::make_unique<Decoder>();
    audioDecoder_  = std::make_unique<Decoder>();
    videoRenderer_ = std::make_unique<VideoRenderer>();
    audioOutput_   = std::make_unique<AudioOutput>(this);
    audioClock_.reset();
    externalClock_.reset();

    // 打开文件
    if (!demuxer_->open(path.toStdString())) {
        emit errorOccurred(QStringLiteral("无法打开文件: %1").arg(path));
        return;
    }

    bool hasVideo = demuxer_->videoIndex() >= 0;
    bool hasAudio = demuxer_->audioIndex() >= 0;

    if (!hasVideo && !hasAudio) {
        emit errorOccurred(QStringLiteral("文件中没有可播放的流"));
        return;
    }

    // 打开视频解码器
    if (hasVideo) {
        auto* codecpar = demuxer_->formatCtx()->streams[demuxer_->videoIndex()]->codecpar;
        if (!videoDecoder_->open(codecpar, hwDecodeEnabled_)) {
            LOG_I("Video decoder open (hw=%d) failed, will retry software", hwDecodeEnabled_);
            // 如果硬解失败自动重试软解
            if (hwDecodeEnabled_) {
                LOG_I("Retrying video decoder with software decoding");
                hwDecodeEnabled_ = false;
                if (!videoDecoder_->open(codecpar, false)) {
                    emit errorOccurred(QStringLiteral("无法初始化视频解码器"));
                    return;
                }
            } else {
                emit errorOccurred(QStringLiteral("无法初始化视频解码器"));
                return;
            }
        }
        if (!videoRenderer_->open(codecpar->width, codecpar->height,
                                  (AVPixelFormat)codecpar->format)) {
            emit errorOccurred(QStringLiteral("无法初始化视频渲染器"));
            return;
        }
    }

    // 打开音频解码器 + 音频输出
    if (hasAudio) {
        auto* codecpar = demuxer_->formatCtx()->streams[demuxer_->audioIndex()]->codecpar;
        // 使用不支持硬解的音频解码器（强制软解）
        if (!audioDecoder_->open(codecpar, false)) {
            LOG_W("Failed to open audio decoder, continuing without audio");
            hasAudio = false;
        } else {
            if (!audioOutput_->init(codecpar->sample_rate,
                                    &codecpar->ch_layout,
                                    (AVSampleFormat)codecpar->format,
                                    &audioClock_)) {
                LOG_W("Failed to init audio output, continuing without audio");
                hasAudio = false;
            } else {
                // 连接均衡器
                audioOutput_->setEqualizer(equalizer_.get());
                audioOutput_->setSpectrumAnalyzer(spectrumAnalyzer_.get());
                int outRate = audioOutput_->outputSampleRate();
                equalizer_->setSampleRate(outRate);
                spectrumAnalyzer_->setSampleRate(outRate);
            }
        }
    }

    emit durationChanged(duration());

    // 新文件：清掉上一曲目的 EQ IIR 残留与频谱累积（工作线程启动前，无竞争）
    equalizer_->clear();
    spectrumAnalyzer_->reset();

    state_ = PlayState::Playing;
    clockAnchorPending_ = true;
    audioDiscardUntilSec_.store(-1.0);
    videoPreroll_.store(false);
    startThreads();

    if (hasAudio) {
        if (hasVideo) {
            // 音视频同步预滚：等首帧渲染时再启动音频设备，避免设备先于视频
            // 拉走一批数据、音频时钟开场就领先造成视频持续追赶丢帧
            audioStartPending_ = true;
            audioStartTp_ = std::chrono::steady_clock::now();
        } else {
            audioOutput_->start();
        }
    }

    renderTimer_->start(15);

    emit stateChanged(state_);
}

void PlayerController::play() {
    if (state_ == PlayState::Paused) {
        if (ended_.load()) {
            // 自然播放结束后的“播放”：从头重播（主流播放器行为）。
            // Seek 命令会顺带把状态恢复为 Playing 并 resume 音频设备。
            LOG_I("Play pressed at end, restarting from beginning");
            ended_ = false;
            seek(0.0);
            return;
        }
        state_ = PlayState::Playing;
        audioClock_.set(audioClock_.now()); // 确保时钟连续
        externalClock_.resume();
        audioOutput_->resume();
        emit stateChanged(state_);
    }
}

void PlayerController::pause() {
    if (state_ == PlayState::Playing) {
        state_ = PlayState::Paused;
        audioClock_.set(audioClock_.now()); // 冻结时钟
        externalClock_.pause();
        audioOutput_->pause();
        emit stateChanged(state_);
    }
}

void PlayerController::stop() {
    if (state_ == PlayState::Idle || state_ == PlayState::Stopped) return;
    postCommand({CommandType::Stop});
}

void PlayerController::shutdown() {
    LOG_I("shutdown: synchronous stop before close");
    renderTimer_->stop();
    state_ = PlayState::Stopped;

    // 两步关闭，避免“join 音频线程”与“释放它正在用的 swr/设备”之间竞态：
    //  1) beginShutdown：停设备回调 + 推进 PCM 代际（唤醒阻塞在入队的生产者），
    //     但保留 swr/QAudioSink 对象；
    //  2) stopThreads join 全部工作线程（音频线程被代际唤醒后安全退出）；
    //  3) join 完成后再释放音频资源。
    if (audioOutput_) audioOutput_->beginShutdown();
    stopThreads();
    if (audioOutput_) audioOutput_->finishShutdown();
}

void PlayerController::seek(double seconds) {
    LOG_I("seek requested: %.3f, state=%d", seconds, static_cast<int>(state_.load()));
    // Idle 无文件；Stopped 时工作线程已退出，命令无人消费
    if (state_ == PlayState::Idle || state_ == PlayState::Stopped) return;
    postCommand({CommandType::Seek, seconds});
}

void PlayerController::setSpeed(double factor) {
    postCommand({CommandType::SetSpeed, factor});
}

void PlayerController::setVolume(int percent) {
    // 音频输出与 UI 同在主线程，直接转发即可
    if (audioOutput_) audioOutput_->setVolume(percent);
}

void PlayerController::setMuted(bool muted) {
    if (audioOutput_) audioOutput_->setMuted(muted);
}

void PlayerController::setVideoTargetSize(int w, int h) {
    if (videoRenderer_) videoRenderer_->setTargetSize(w, h);
}

void PlayerController::stepForward() {
    // 仅在暂停态有效
    if (state_ != PlayState::Paused) {
        LOG_W("stepForward ignored: state is not Paused");
        return;
    }
    // 末帧暂停态管线已枯竭且不应自动重启
    if (ended_.load()) {
        LOG_W("stepForward ignored: playback ended");
        return;
    }

    LOG_I("stepForward: queueSize=%zu, position=%.3f", frameQueue_.size(), position());

    // 暂停前 onRenderTick 可能暂存了一帧"超前帧"，它就是下一帧，优先消费
    AVFrame* frame = nullptr;
    {
        std::lock_guard<std::mutex> lock(pendingFrameMtx_);
        if (pendingEarlyFrame_) {
            frame = pendingEarlyFrame_;
            pendingEarlyFrame_ = nullptr;
        }
    }

    // 从帧队列取下一帧；队列为空时循环等待解码线程产生帧
    for (int retry = 0; retry < 20 && !frame; ++retry) {
        if (frameQueue_.pop(frame, std::chrono::milliseconds(50))) {
            break;
        }
        // 如果线程已停止，不再等待
        if (!threadsRunning_) {
            LOG_W("stepForward: threads stopped while waiting");
            return;
        }
    }

    // 管线枯竭（demuxer 读取完毕、所有帧已消费）：主动 seek 重启管线
    if (!frame) {
        double pos = position();
        double target = std::min(pos + 0.5, duration());
        LOG_I("stepForward: pipeline dry, seeking to %.3f to restart", target);
        postCommand({CommandType::Seek, target});

        // 等待 seek 后的新帧
        for (int retry = 0; retry < 30; ++retry) {
            if (frameQueue_.pop(frame, std::chrono::milliseconds(50))) break;
            if (!threadsRunning_) return;
        }
    }

    if (!frame) {
        LOG_W("stepForward: frameQueue empty or timeout (retried %d ms)", 20 * 50);
        return;
    }

    int64_t pts = frame->best_effort_timestamp;
    QImage img = videoRenderer_->render(frame);
    double before = position();
    // 无音频流时，把时间同步到当前帧 PTS（与首帧锚定等价，消费掉 pending 标志，
    // 避免恢复播放时 onRenderTick 再次锚定造成回跳）
    if (demuxer_ && demuxer_->audioIndex() < 0 && pts != AV_NOPTS_VALUE) {
        double ptsSec = pts * av_q2d(demuxer_->videoTimeBase());
        externalClock_.set(ptsSec);
        clockAnchorPending_ = false;
    }
    LOG_I("stepForward: got frame pts=%lld, render ok=%d, position before=%.3f, after=%.3f",
          pts, !img.isNull(), before, position());
    av_frame_free(&frame);
    if (!img.isNull()) {
        emit frameReady(img);
    }
}

void PlayerController::stepBackward() {
    if (state_ != PlayState::Paused || !demuxer_) {
        LOG_W("stepBackward ignored: state=%d, hasDemuxer=%s", static_cast<int>(state_.load()), demuxer_ ? "yes" : "no");
        return;
    }

    // seek 到当前位置前 1 秒，让解码器重新解码到目标帧
    double pos = position();
    double target = std::max(0.0, pos - 1.0);
    LOG_I("stepBackward: current=%.3f, target=%.3f", pos, target);
    seek(target);
}

double PlayerController::position() const {
    // 有音频流：使用音频时钟；无音频流：使用外部时钟
    if (demuxer_ && demuxer_->audioIndex() >= 0) {
        return audioClock_.now();
    }
    return externalClock_.now();
}

double PlayerController::duration() const {
    return demuxer_ ? demuxer_->durationSec() : 0.0;
}

int PlayerController::videoWidth() const {
    if (!demuxer_ || demuxer_->videoIndex() < 0) return 0;
    return demuxer_->formatCtx()->streams[demuxer_->videoIndex()]->codecpar->width;
}

int PlayerController::videoHeight() const {
    if (!demuxer_ || demuxer_->videoIndex() < 0) return 0;
    return demuxer_->formatCtx()->streams[demuxer_->videoIndex()]->codecpar->height;
}

// ══════════════════════════════════════════════════════════════════════════
//  Render Timer (主线程)
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::onRenderTick() {
    if (state_ != PlayState::Playing) return; // 暂停态不消耗帧，由 stepForward 处理

    // 丢帧统计（每秒最多输出一条，便于诊断同步问题）
    std::chrono::steady_clock::time_point now;
    {
        now = std::chrono::steady_clock::now();
        if (now - lastStatTime_ >= std::chrono::seconds(1)) {
            if (statDroppedLate_ > 0) {
                LOG_W("Dropped %d late frame(s) in the last second", statDroppedLate_);
            }
            statDroppedLate_ = 0;
            lastStatTime_ = now;
        }
    }

    // 兜底：视频流损坏/迟迟出不了首帧时，不能让音频永远挂起不启动
    if (audioStartPending_ && audioOutput_ && audioOutput_->isInitialized() &&
        now - audioStartTp_ > std::chrono::seconds(2)) {
        LOG_W("First video frame timeout, starting audio anyway");
        audioOutput_->start();
        audioStartPending_ = false;
    }

    // 自然播放结束判定（不依赖本次是否取到帧，每次 tick 都检查）
    if (checkPlaybackFinished()) return;

    // 先检查前一 tick 留下的"超前"帧是否已就绪
    AVFrame* frame = nullptr;
    {
        std::lock_guard<std::mutex> lock(pendingFrameMtx_);
        if (pendingEarlyFrame_) {
            double delay = calcVideoDelay(pendingEarlyFrame_);
            if (delay > 0.040) {
                // 仍然超前，继续等待
                return;
            }
            // 已达到展示窗口，取走所有权
            frame = pendingEarlyFrame_;
            pendingEarlyFrame_ = nullptr;
        }
    }

    // 没有待处理帧，正常从队列取
    if (!frame) {
        if (!frameQueue_.pop(frame, std::chrono::milliseconds(0))) {
            return;
        }
        if (!frame) return;
    }

    // 打开/seek 后首帧：将外部时钟锚定到该帧 PTS（无音频流时），
    // 消除解码预热期时钟空跑造成的开头丢帧。有音频流时音频是主时钟，
    // 视频按既有追赶逻辑追上即可。
    if (clockAnchorPending_) {
        // seek 后第一批帧可能是目标点之前的关键帧（pre-roll），不能拿它们锚定，
        // 否则旧帧会被当作准时帧直接显示（画面可见地回跳一段）。
        // 只对已进入显示窗口的帧锚定。
        double anchorDelay = calcVideoDelay(frame);
        if (anchorDelay >= -0.120) {
            clockAnchorPending_ = false;
            if (demuxer_ && demuxer_->audioIndex() < 0 &&
                frame->pts != AV_NOPTS_VALUE) {
                externalClock_.set(frame->pts * av_q2d(demuxer_->videoTimeBase()));
            }
        }
    }

    // 音视频同步判断
    if (state_ == PlayState::Playing) {
        double delay = calcVideoDelay(frame);

        if (delay > 0.040) { // 提前超过 40ms：暂存，下一 tick 重试
            std::lock_guard<std::mutex> lock(pendingFrameMtx_);
            // clearQueues 可能已在解复用线程释放过，防御性检查
            if (!pendingEarlyFrame_) {
                pendingEarlyFrame_ = frame;
            } else {
                av_frame_free(&frame);
            }
            return;
        }

        // 丢帧阈值取 120ms（参考 ffplay 的 ~110ms）：音频时钟按"已交给
        // 后端设备的样本数"推进，天然领先实际可听声音约一个设备缓冲周期
        // （PulseAudio 下实测 ~80~100ms），阈值过小会把正常帧误判为落后。
        if (delay < -0.120) { // 落后超过 120ms：丢帧
            if (videoPreroll_.load()) {
                // seek 后关键帧追赶期的 pre-roll：预期内丢弃，不计 late 统计
                av_frame_free(&frame);
            } else {
                statDroppedLate_++;
                av_frame_free(&frame);
            }

            // 再尝试取下一帧
            if (frameQueue_.pop(frame, std::chrono::milliseconds(0))) {
                if (!frame) return;
                // 检查这帧是否也落后太多
                delay = calcVideoDelay(frame);
                if (delay < -0.120) {
                    if (!videoPreroll_.load()) statDroppedLate_++;
                    av_frame_free(&frame);
                    return;
                }
            } else {
                return;
            }
        }
        // -120ms ~ +40ms：正常显示，pre-roll 追赶结束
        videoPreroll_.store(false);
    }

    // 渲染前根据 SAR 更新显示宽高比（sws 目标尺寸由 UI 按该比例下发）
    updateDisplayAspect(frame);

    QImage img = videoRenderer_->render(frame);
    av_frame_free(&frame);

    if (!img.isNull()) {
        if (frameCount_ == 0) {
            // 首帧已上屏：启动预滚挂起的音频设备
            if (audioStartPending_ && audioOutput_ && audioOutput_->isInitialized()) {
                audioOutput_->start();
                audioStartPending_ = false;
            }
            emit firstFrameRendered();
        }
        frameCount_++;
        emit frameReady(img);
    }
}

double PlayerController::calcVideoDelay(AVFrame* frame) {
    // 计算视频帧 PTS 与音频时钟的差值
    if (!demuxer_ || demuxer_->videoIndex() < 0) return 0.0;

    AVRational tb = demuxer_->videoTimeBase();
    double pts = frame->pts * av_q2d(tb);

    double clock;
    if (demuxer_->audioIndex() >= 0) {
        clock = audioClock_.now();
    } else {
        clock = externalClock_.now();
    }

    return pts - clock;
}

// ══════════════════════════════════════════════════════════════════════════
//  EOF / 结束判定（主线程）
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::resetEofState() {
    demuxEof_.store(false);
    audioDrainEof_.store(false);
    videoDrainEof_.store(false);
    ended_.store(false);
    eofSettling_ = false;
}

bool PlayerController::checkPlaybackFinished() {
    if (ended_.load()) return true;
    if (!demuxEof_.load()) return false;

    const bool hasAudio = demuxer_ && demuxer_->audioIndex() >= 0;
    const bool hasVideo = demuxer_ && demuxer_->videoIndex() >= 0;

    // 等待两条流各自 drain 完成、所有已解出的数据上屏/交设备
    if (hasAudio) {
        if (!audioDrainEof_.load()) return false;
        if (audioOutput_ && !audioOutput_->isBufferEmpty()) return false;
    }
    if (hasVideo) {
        if (!videoDrainEof_.load()) return false;
        if (!frameQueue_.empty()) return false;
        {
            std::lock_guard<std::mutex> lock(pendingFrameMtx_);
            if (pendingEarlyFrame_) return false;
        }
    }

    // 设备缓冲中还有尾音：用宽松时钟阈值 + 250ms 稳态等待确保不截尾音
    double dur = duration();
    double pos = position();
    double lead = hasAudio ? 0.25 : 0.10;
    if (dur > 0.0 && pos < dur - lead) {
        eofSettling_ = false;
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    if (!eofSettling_) {
        eofSettling_ = true;
        eofSettleTp_ = now;
        return false;
    }
    if (now - eofSettleTp_ < std::chrono::milliseconds(250)) return false;

    enterPlaybackFinished();
    return true;
}

void PlayerController::enterPlaybackFinished() {
    LOG_I("Playback finished, entering end-paused mode");
    eofSettling_ = false;
    ended_ = true;
    state_ = PlayState::Paused;

    // 时钟吸附到总时长；纯视频外部时钟冻结
    double dur = duration();
    externalClock_.pause();
    externalClock_.set(dur);
    if (demuxer_ && demuxer_->audioIndex() >= 0) {
        audioClock_.set(dur);
        if (audioOutput_) audioOutput_->pause(); // suspend，停止欠载填零
    }

    emit stateChanged(PlayState::Paused);
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
//  Demux Thread
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::demuxLoop() {
    LOG_I("DemuxThread started");
    threadsRunning_ = true;

    // 尚未交付给包队列的 packet（目标队列暂时满时留到下一轮重试，
    // 本线程期间仍可持续处理命令/不会饿死另一条流）
    AVPacket* pendingPkt = nullptr;
    // EOF 后 nullptr drain 哨兵是否已投递（seek 后复位）
    bool eofSentinelsSent = false;
    const bool hasA = demuxer_->audioIndex() >= 0;
    const bool hasV = demuxer_->videoIndex() >= 0;

    while (threadsRunning_) {
        // 1. 命令优先处理（保证 seek/stop 最多一个轮询周期即响应）
        Command cmd;
        if (cmdQueue_.pop(cmd, std::chrono::milliseconds(0))) {
            if ((cmd.type == CommandType::Seek ||
                 cmd.type == CommandType::Stop ||
                 cmd.type == CommandType::Quit) && pendingPkt) {
                av_packet_free(&pendingPkt);
            }
            processCommand(cmd);
            if (cmd.type == CommandType::Seek) {
                // av_seek_frame 已清除 libavformat 的 EOF 状态，恢复读包
                eofSentinelsSent = false;
            }
            if (cmd.type == CommandType::Quit || cmd.type == CommandType::Stop) break;
        }

        // 仅在播放或暂停态继续解复用；暂停时维持少量 packet 缓冲供逐帧使用
        if (state_ != PlayState::Playing && state_ != PlayState::Paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 2. 水位/背压门控
        if (state_ == PlayState::Paused) {
            // 暂停态只维持低水位预读：保证 seek/逐帧后能迅速取到包解码，
            // 又不在队列里积压数秒数据——否则恢复瞬间音频解码线程会把积压
            // 一口气灌进容量有限的 PCM 队列，造成 "PCM buffer full" 丢帧
            if (audioPacketQueue_.size() >= 10 || videoPacketQueue_.size() >= 10) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
        } else {
            // 播放态：所有“存在的流”的包队列都满才等待。
            // 注意纯音频文件视频队列恒空，不能要求视频队列也满。
            bool audioFull = !hasA ||
                audioPacketQueue_.size() >= audioPacketQueue_.capacity();
            bool videoFull = !hasV ||
                videoPacketQueue_.size() >= videoPacketQueue_.capacity();
            if (audioFull && videoFull) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
        }

        // 3. EOF：向存在的解码器各投一个 nullptr 哨兵触发 drain。
        //    队列可能仍满，必须两个哨兵都入队成功才能标记完成，
        //    否则下一轮重试（丢标记会导致解码器永不 drain、结束判定失效）。
        if (demuxEof_.load()) {
            if (!eofSentinelsSent) {
                bool ok = true;
                if (hasA) ok &= audioPacketQueue_.push(nullptr,
                                                       std::chrono::milliseconds(5));
                if (hasV) ok &= videoPacketQueue_.push(nullptr,
                                                       std::chrono::milliseconds(5));
                if (ok) eofSentinelsSent = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 4. 读下一个包（仅当没有持有未交付包时）
        if (!pendingPkt) {
            ReadStatus st = demuxer_->readPacket(pendingPkt);
            if (st != ReadStatus::Ok) {
                pendingPkt = nullptr;
                LOG_I("Demux %s reached", st == ReadStatus::Eof ? "EOF" : "error");
                // 读错误同样进入 drain：已缓冲的数据仍可播完
                demuxEof_.store(true);
                continue;
            }
        }

        // 5. 短超时投递；失败则保留 packet 下轮重试（队头阻塞 ≤5ms）
        int idx = pendingPkt->stream_index;
        if (idx == demuxer_->audioIndex()) {
            if (!audioPacketQueue_.push(std::move(pendingPkt),
                                        std::chrono::milliseconds(5))) {
                continue;
            }
        } else if (idx == demuxer_->videoIndex()) {
            if (!videoPacketQueue_.push(std::move(pendingPkt),
                                        std::chrono::milliseconds(5))) {
                continue;
            }
        } else {
            av_packet_free(&pendingPkt);
        }
        pendingPkt = nullptr;
    }

    if (pendingPkt) av_packet_free(&pendingPkt);
    LOG_I("DemuxThread stopped");
    threadsRunning_ = false;
}

// ══════════════════════════════════════════════════════════════════════════
//  Audio Decode Thread
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::audioDecodeLoop() {
    LOG_I("AudioDecodeThread started");

    // 本线程内统计每次 seek 丢弃的 pre-roll 帧数（仅诊断用）
    double lastDiscardTarget = -1.0;
    int prerollDiscarded = 0;

    // AVFrame 在线程内复用，避免每帧 alloc/free
    AVFrame* frame = av_frame_alloc();
    // 记录上次见到的 PCM 代际：seek 清空 PCM 后据此重置 swr 内部延迟
    uint64_t lastPcmGen = audioOutput_ ? audioOutput_->queueGeneration() : 0;

    while (threadsRunning_ && frame) {
        if (state_ != PlayState::Playing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // PCM 代际变化（seek/stop）：swr 内部还缓存着旧位置样本，必须重置
        if (audioOutput_) {
            uint64_t gen = audioOutput_->queueGeneration();
            if (gen != lastPcmGen) {
                audioOutput_->resetPipeline();
                lastPcmGen = gen;
            }
        }

        AVPacket* pkt = nullptr;
        if (!audioPacketQueue_.pop(pkt, std::chrono::milliseconds(50))) {
            continue;
        }

        // EOF drain 哨兵：发送 NULL 包排空解码器延迟帧，再 flush swr 尾部
        if (pkt == nullptr) {
            uint64_t seekGen = seekGeneration_.load();
            audioDecoder_->sendPacket(nullptr);
            while (threadsRunning_) {
                int ret = audioDecoder_->receiveFrame(frame);
                if (ret != 0) break;
                bool discard = handleAudioPreroll(frame, lastDiscardTarget,
                                                  prerollDiscarded);
                if (!discard && audioOutput_ && audioOutput_->isInitialized()) {
                    audioOutput_->writeFrame(frame);
                }
                av_frame_unref(frame);
            }
            if (audioOutput_) audioOutput_->flushTail();
            // drain 期间若发生 seek，本次 EOF 结果作废（标志以 seek 复位为准）
            if (seekGeneration_.load() == seekGen) {
                audioDrainEof_.store(true);
                LOG_I("Audio decoder drained to EOF");
            }
            continue;
        }

        int ret = audioDecoder_->sendPacket(pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            LOG_W("Audio sendPacket error: %d", ret);
        }
        av_packet_free(&pkt);

        // 接收所有可用帧
        while (threadsRunning_) {
            ret = audioDecoder_->receiveFrame(frame);
            if (ret != 0) break;
            // seek pre-roll：整帧结束时间仍早于目标点的 PCM 丢弃
            bool discard = handleAudioPreroll(frame, lastDiscardTarget,
                                              prerollDiscarded);
            if (!discard && audioOutput_ && audioOutput_->isInitialized()) {
                audioOutput_->writeFrame(frame);
            }
            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    LOG_I("AudioDecodeThread stopped");
}

bool PlayerController::handleAudioPreroll(AVFrame* frame, double& lastDiscardTarget,
                                          int& prerollDiscarded) {
    double discardUntil = audioDiscardUntilSec_.load();
    if (discardUntil < 0.0) return false;

    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = frame->pts;
    if (pts == AV_NOPTS_VALUE) return false; // 无时间戳时保守播放，避免误丢整段

    AVRational atb = demuxer_->audioTimeBase();
    int64_t dur = av_rescale_q(frame->nb_samples,
                               AVRational{1, frame->sample_rate}, atb);
    double endSec = (pts + dur) * av_q2d(atb);
    if (endSec < discardUntil) {
        if (discardUntil != lastDiscardTarget) {
            lastDiscardTarget = discardUntil;
            prerollDiscarded = 0;
        }
        prerollDiscarded++;
        return true;
    }

    if (lastDiscardTarget >= 0.0) {
        LOG_I("seek: discarded %d audio preroll frames before %.3f",
              prerollDiscarded, lastDiscardTarget);
        lastDiscardTarget = -1.0;
        prerollDiscarded = 0;
    }
    audioDiscardUntilSec_.store(-1.0);
    return false;
}

// ══════════════════════════════════════════════════════════════════════════
//  Video Decode Thread
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::videoDecodeLoop() {
    LOG_I("VideoDecodeThread started");

    // 注意：frameQueue_ 持有的是独立 AVFrame*（主线程逐个 av_frame_free），
    // 因此入队的帧必须各自 av_frame_alloc，不能像音频帧那样复用同一对象。
    // 取一帧并入队；满则按“保旧帧”策略丢弃；返回 receiveFrame 的结果
    auto receiveAndEnqueue = [&](int& outRet) {
        AVFrame* f = av_frame_alloc();
        if (!f) { outRet = AVERROR(ENOMEM); return; }
        outRet = videoDecoder_->receiveFrame(f);
        if (outRet != 0) {
            av_frame_free(&f);
            return;
        }
        if (state_ == PlayState::Playing &&
            frameQueue_.size() >= frameQueue_.capacity()) {
            av_frame_free(&f); // 队列满：丢新帧保旧帧
        } else if (!frameQueue_.push(std::move(f),
                                     std::chrono::milliseconds(200))) {
            av_frame_free(&f);
        }
    };

    while (threadsRunning_) {
        // 播放或暂停态都继续解码；暂停时只预解码几帧供 stepForward 使用
        if (state_ != PlayState::Playing && state_ != PlayState::Paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 暂停态：保留最多 5 帧缓冲供 stepForward 使用
        if (state_ == PlayState::Paused && frameQueue_.size() >= 5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 播放态：帧队列已有足够缓冲时等待（防止解码器跑在实时前面太远）
        if (state_ == PlayState::Playing && frameQueue_.size() >= 8) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        AVPacket* pkt = nullptr;
        if (!videoPacketQueue_.pop(pkt, std::chrono::milliseconds(50))) {
            continue;
        }

        // EOF drain 哨兵：发送 NULL 包取出解码器尾部延迟帧
        if (pkt == nullptr) {
            uint64_t seekGen = seekGeneration_.load();
            videoDecoder_->sendPacket(nullptr);
            while (threadsRunning_) {
                int ret;
                receiveAndEnqueue(ret);
                if (ret != 0) break;
            }
            if (seekGeneration_.load() == seekGen) {
                videoDrainEof_.store(true);
            }
            continue;
        }

        int ret = videoDecoder_->sendPacket(pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            LOG_W("Video sendPacket error: %d", ret);
        }
        av_packet_free(&pkt);

        while (threadsRunning_) {
            int r;
            receiveAndEnqueue(r);
            if (r != 0) break;
        }
    }

    LOG_I("VideoDecodeThread stopped");
}

// ══════════════════════════════════════════════════════════════════════════
//  Command Handling
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::postCommand(Command cmd) {
    cmdQueue_.push(std::move(cmd), std::chrono::milliseconds(100));
}

void PlayerController::processCommand(const Command& cmd) {
    switch (cmd.type) {
    case CommandType::Play: {
        if (state_ == PlayState::Paused) {
            state_ = PlayState::Playing;
            audioClock_.set(audioClock_.now());
            externalClock_.resume();
            // QAudioSink 属于主线程，跨线程调用需排队；QPointer 防止 reopen 后对象已替换
            QPointer<AudioOutput> ao = audioOutput_.get();
            QMetaObject::invokeMethod(this, [ao] { if (ao) ao->resume(); },
                                      Qt::QueuedConnection);
            emit stateChanged(state_);
        }
        break;
    }
    case CommandType::Pause: {
        if (state_ == PlayState::Playing) {
            state_ = PlayState::Paused;
            audioClock_.set(audioClock_.now());
            externalClock_.pause();
            QPointer<AudioOutput> ao = audioOutput_.get();
            QMetaObject::invokeMethod(this, [ao] { if (ao) ao->pause(); },
                                      Qt::QueuedConnection);
            emit stateChanged(state_);
        }
        break;
    }
    case CommandType::Stop: {
        state_ = PlayState::Stopped;
        resetEofState();
        // renderTimer_ / QAudioSink 只能在其所属的主线程操作
        QPointer<AudioOutput> ao = audioOutput_.get();
        QMetaObject::invokeMethod(this, [this, ao] {
            renderTimer_->stop();
            if (ao) ao->stop();
        }, Qt::QueuedConnection);
        clearQueues();
        emit stateChanged(state_);
        break;
    }
    case CommandType::Seek: {
        if (!demuxer_) break;
        double target = cmd.arg;
        double dur = duration();
        if (dur > 0.0) target = std::clamp(target, 0.0, dur);
        LOG_I("Seek: target=%.3f (state=%d)", target, static_cast<int>(state_.load()));

        const int vIdx = demuxer_->videoIndex();
        const int aIdx = demuxer_->audioIndex();

        // seek：有视频流时以视频时间基定位视频流、以音频时间基定位音频流；
        // 纯音频文件只 seek 音频流（此前被 videoIndex>=0 条件整体跳过）
        if (vIdx >= 0) {
            AVRational vtb = demuxer_->videoTimeBase();
            int64_t vts = (int64_t)(target / av_q2d(vtb));
            av_seek_frame(demuxer_->formatCtx(), vIdx, vts, AVSEEK_FLAG_BACKWARD);
            if (aIdx >= 0) {
                AVRational atb = demuxer_->audioTimeBase();
                int64_t ats = (int64_t)(target / av_q2d(atb));
                av_seek_frame(demuxer_->formatCtx(), aIdx, ats, AVSEEK_FLAG_BACKWARD);
            }
        } else if (aIdx >= 0) {
            AVRational atb = demuxer_->audioTimeBase();
            int64_t ats = (int64_t)(target / av_q2d(atb));
            av_seek_frame(demuxer_->formatCtx(), aIdx, ats, AVSEEK_FLAG_BACKWARD);
        }

        // flush 解码器（Decoder 内部加锁，与解码线程的 receive 串行）
        if (videoDecoder_) videoDecoder_->flush();
        if (audioDecoder_) audioDecoder_->flush();

        clearQueues();
        if (audioOutput_) audioOutput_->clear();

        // 重置时钟到目标位置
        audioClock_.set(target);
        externalClock_.set(target);
        // seek 后首帧重新锚定外部时钟（视频-only）
        clockAnchorPending_ = true;
        // 关键帧到目标点之间的 pre-roll 一律跳过，不能播旧声/旧画
        audioDiscardUntilSec_.store(aIdx >= 0 ? target : -1.0);
        videoPreroll_.store(vIdx >= 0);

        // EOF/drain 状态复位；seekGeneration 让在途 drain 结果作废
        ended_ = false;
        eofSettling_ = false;
        demuxEof_.store(false);
        audioDrainEof_.store(false);
        videoDrainEof_.store(false);
        seekGeneration_.fetch_add(1);

        // 暂停态（含手动暂停与末帧暂停）下 seek：恢复播放
        if (state_ == PlayState::Paused) {
            state_ = PlayState::Playing;
            audioClock_.set(audioClock_.now()); // 确保时钟连续
            externalClock_.resume();
            QPointer<AudioOutput> ao = audioOutput_.get();
            QMetaObject::invokeMethod(this, [ao] { if (ao) ao->resume(); },
                                      Qt::QueuedConnection);
            emit stateChanged(state_);
        }
        break;
    }
    case CommandType::SetSpeed:
        speed_ = cmd.arg;
        if (speed_ < 0.5) speed_ = 0.5;
        if (speed_ > 2.0) speed_ = 2.0;
        // 实际执行速度变更：音频通过改变输出采样率（swr）实现变调变速；
        // 纯视频通过外部时钟速率调节
        if (audioOutput_) audioOutput_->setPlaybackSpeed(speed_);
        externalClock_.setSpeed(speed_);
        LOG_I("Playback speed set to %.2f, audioOutput=%s",
              speed_, audioOutput_ ? "yes" : "no");
        emit speedChanged(speed_);
        break;
    case CommandType::StepForward:
        // 在 audioDecodeLoop 中无法直接处理逐帧
        // 由主线程的 stepForward() 直接处理
        break;
    case CommandType::Quit:
        threadsRunning_ = false;
        break;
    }
}

// ══════════════════════════════════════════════════════════════════════════
//  Thread Management
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::startThreads() {
    stopThreads();
    clearQueues();

    threadsRunning_ = true;

    demuxThread_       = std::thread(&PlayerController::demuxLoop, this);
    audioDecodeThread_ = std::thread(&PlayerController::audioDecodeLoop, this);
    videoDecodeThread_ = std::thread(&PlayerController::videoDecodeLoop, this);

    LOG_I("Threads started (3 threads)");
}

void PlayerController::stopThreads() {
    threadsRunning_ = false;

    // 先按元素释放再清空。注意不能只调队列的 clear()：它只清容器、不释放
    // 其中的 AVPacket/AVFrame 指针（曾导致关闭时泄漏约 200 个包/帧）。
    // clearQueues 内部的 pop 会唤醒阻塞在 push/pop 上的工作线程，使其
    // 及时退出；join 后再清一次处理线程退出前产生的残留。
    clearQueues();

    if (demuxThread_.joinable())       demuxThread_.join();
    if (audioDecodeThread_.joinable()) audioDecodeThread_.join();
    if (videoDecodeThread_.joinable()) videoDecodeThread_.join();

    clearQueues();
    LOG_I("Threads stopped");
}

void PlayerController::clearQueues() {
    auto clearPktQueue = [](BoundedQueue<AVPacket*>& q) {
        AVPacket* pkt = nullptr;
        while (q.pop(pkt, std::chrono::milliseconds(0))) {
            if (pkt) av_packet_free(&pkt);
        }
        q.clear();
    };

    auto clearFrameQueue = [](BoundedQueue<AVFrame*>& q) {
        AVFrame* f = nullptr;
        while (q.pop(f, std::chrono::milliseconds(0))) {
            if (f) av_frame_free(&f);
        }
        q.clear();
    };

    clearPktQueue(audioPacketQueue_);
    clearPktQueue(videoPacketQueue_);
    clearFrameQueue(frameQueue_);
    cmdQueue_.clear();

    // 清理暂存的超前帧（可能与主线程 onRenderTick 并发，需加锁）
    {
        std::lock_guard<std::mutex> lock(pendingFrameMtx_);
        if (pendingEarlyFrame_) {
            av_frame_free(&pendingEarlyFrame_);
            pendingEarlyFrame_ = nullptr;
        }
    }
}
