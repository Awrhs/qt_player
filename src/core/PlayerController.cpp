#include "PlayerController.h"
#include "../utils/Log.h"

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
            return;
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
                                    codecpar->ch_layout.nb_channels,
                                    codecpar->ch_layout.u.mask,
                                    &audioClock_)) {
                LOG_W("Failed to init audio output, continuing without audio");
                hasAudio = false;
            } else {
                // 连接均衡器
                audioOutput_->setEqualizer(equalizer_.get());
                audioOutput_->setSpectrumAnalyzer(spectrumAnalyzer_.get());
                equalizer_->setSampleRate(audioOutput_->outputSampleRate());
            }
        }
    }

    emit durationChanged(duration());

    state_ = PlayState::Playing;
    startThreads();

    if (hasAudio) {
        audioOutput_->start();
    }

    renderTimer_->start(15);

    emit stateChanged(state_);
}

void PlayerController::play() {
    if (state_ == PlayState::Paused) {
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

void PlayerController::seek(double seconds) {
    if (state_ == PlayState::Idle) return;
    postCommand({CommandType::Seek, seconds});
}

void PlayerController::setSpeed(double factor) {
    postCommand({CommandType::SetSpeed, factor});
}

void PlayerController::stepForward() {
    // 仅在暂停态有效
    if (state_ != PlayState::Paused) return;

    // 从帧队列取下一帧
    AVFrame* frame = nullptr;
    if (frameQueue_.pop(frame, std::chrono::milliseconds(100))) {
        if (frame) {
            QImage img = videoRenderer_->render(frame);
            av_frame_free(&frame);
            if (!img.isNull()) {
                emit frameReady(img);
            }
        }
    }
}

void PlayerController::stepBackward() {
    if (state_ != PlayState::Paused || !demuxer_) return;

    // seek 到当前位置前 1 秒，让解码器重新解码到目标帧
    double pos = position();
    double target = std::max(0.0, pos - 1.0);
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

// ══════════════════════════════════════════════════════════════════════════
//  Render Timer (主线程)
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::onRenderTick() {
    if (state_ != PlayState::Playing && state_ != PlayState::Paused) return;

    // 取帧
    AVFrame* frame = nullptr;
    if (!frameQueue_.pop(frame, std::chrono::milliseconds(0))) {
        return;
    }
    if (!frame) return;

    // 音视频同步判断
    if (state_ == PlayState::Playing) {
        double delay = calcVideoDelay(frame);

        if (delay > 0.040) { // 提前超过 40ms：等待
            // 把帧放回去（放回队首）
            // 简化：暂不支持放回，直接跳过该帧
            av_frame_free(&frame);
            return;
        }

        if (delay < -0.080) { // 落后超过 80ms：丢帧
            av_frame_free(&frame);

            // 再尝试取下一帧
            if (frameQueue_.pop(frame, std::chrono::milliseconds(0))) {
                if (!frame) return;
                // 检查这帧是否也落后太多
                delay = calcVideoDelay(frame);
                if (delay < -0.080) {
                    av_frame_free(&frame);
                    return;
                }
            } else {
                return;
            }
        }
        // -80ms ~ +40ms：正常显示
    }

    QImage img = videoRenderer_->render(frame);
    av_frame_free(&frame);

    if (!img.isNull()) {
        if (frameCount_ == 0) {
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
//  Demux Thread
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::demuxLoop() {
    LOG_I("DemuxThread started");
    threadsRunning_ = true;

    while (threadsRunning_) {
        // 处理命令
        Command cmd;
        if (cmdQueue_.pop(cmd, std::chrono::milliseconds(0))) {
            processCommand(cmd);
            if (cmd.type == CommandType::Quit || cmd.type == CommandType::Stop) break;
        }

        if (state_ != PlayState::Playing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 背压检查
        bool audioFull = audioPacketQueue_.size() >= audioPacketQueue_.capacity();
        bool videoFull = videoPacketQueue_.size() >= videoPacketQueue_.capacity();
        if (audioFull && videoFull) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        AVPacket* pkt = demuxer_->read();
        if (!pkt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 按流类型路由
        int idx = pkt->stream_index;
        if (idx == demuxer_->audioIndex()) {
            if (!audioPacketQueue_.push(std::move(pkt), std::chrono::milliseconds(500))) {
                av_packet_free(&pkt);
            }
        } else if (idx == demuxer_->videoIndex()) {
            if (!videoPacketQueue_.push(std::move(pkt), std::chrono::milliseconds(500))) {
                av_packet_free(&pkt);
            }
        } else {
            av_packet_free(&pkt);
        }
    }

    LOG_I("DemuxThread stopped");
    threadsRunning_ = false;
}

// ══════════════════════════════════════════════════════════════════════════
//  Audio Decode Thread
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::audioDecodeLoop() {
    LOG_I("AudioDecodeThread started");

    while (threadsRunning_) {
        if (state_ != PlayState::Playing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        AVPacket* pkt = nullptr;
        if (!audioPacketQueue_.pop(pkt, std::chrono::milliseconds(50))) {
            continue;
        }

        int ret = audioDecoder_->sendPacket(pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            LOG_W("Audio sendPacket error: %d", ret);
        }
        av_packet_free(&pkt);

        // 接收所有可用帧
        while (true) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) break;

            ret = audioDecoder_->receiveFrame(frame);
            if (ret == 0) {
                if (audioOutput_ && audioOutput_->isInitialized()) {
                    audioOutput_->writeFrame(frame);
                }
                av_frame_free(&frame);
            } else {
                av_frame_free(&frame);
                if (ret == AVERROR(EAGAIN)) break;
                break;
            }
        }
    }

    LOG_I("AudioDecodeThread stopped");
}

// ══════════════════════════════════════════════════════════════════════════
//  Video Decode Thread
// ══════════════════════════════════════════════════════════════════════════

void PlayerController::videoDecodeLoop() {
    LOG_I("VideoDecodeThread started");

    while (threadsRunning_) {
        if (state_ != PlayState::Playing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 帧队列满 → 可丢帧（视频）
        if (frameQueue_.size() >= frameQueue_.capacity()) {
            // 丢一帧以腾出空间
            AVFrame* old = nullptr;
            if (frameQueue_.pop(old, std::chrono::milliseconds(5))) {
                av_frame_free(&old);
            }
        }

        AVPacket* pkt = nullptr;
        if (!videoPacketQueue_.pop(pkt, std::chrono::milliseconds(50))) {
            continue;
        }

        int ret = videoDecoder_->sendPacket(pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            LOG_W("Video sendPacket error: %d", ret);
        }
        av_packet_free(&pkt);

        while (true) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) break;

            ret = videoDecoder_->receiveFrame(frame);
            if (ret == 0) {
                if (!frameQueue_.push(std::move(frame), std::chrono::milliseconds(200))) {
                    av_frame_free(&frame);
                }
            } else {
                av_frame_free(&frame);
                if (ret == AVERROR(EAGAIN)) break;
                break;
            }
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
    case CommandType::Play:
        if (state_ == PlayState::Paused) {
            state_ = PlayState::Playing;
            audioClock_.set(audioClock_.now());
            externalClock_.resume();
            audioOutput_->resume();
            emit stateChanged(state_);
        }
        break;
    case CommandType::Pause:
        if (state_ == PlayState::Playing) {
            state_ = PlayState::Paused;
            audioClock_.set(audioClock_.now());
            externalClock_.pause();
            audioOutput_->pause();
            emit stateChanged(state_);
        }
        break;
    case CommandType::Stop:
        state_ = PlayState::Stopped;
        renderTimer_->stop();
        audioOutput_->stop();
        clearQueues();
        emit stateChanged(state_);
        break;
    case CommandType::Seek:
        if (demuxer_ && demuxer_->videoIndex() >= 0) {
            double target = cmd.arg;
            // 转为 stream time_base（使用视频时间基）
            AVRational tb = demuxer_->videoTimeBase();
            int64_t ts = (int64_t)(target / av_q2d(tb));
            // 向后 seek 到最近的关键帧
            av_seek_frame(demuxer_->formatCtx(), demuxer_->videoIndex(), ts, AVSEEK_FLAG_BACKWARD);
            if (demuxer_->audioIndex() >= 0) {
                av_seek_frame(demuxer_->formatCtx(), demuxer_->audioIndex(), ts, AVSEEK_FLAG_BACKWARD);
            }

            // flush 解码器
            videoDecoder_->flush();
            if (audioDecoder_) audioDecoder_->flush();

            clearQueues();
            audioOutput_->clear();

            // 重置时钟到目标位置
            audioClock_.set(target);
            externalClock_.set(target);
        }
        break;
    case CommandType::SetSpeed:
        speed_ = cmd.arg;
        if (speed_ < 0.5) speed_ = 0.5;
        if (speed_ > 2.0) speed_ = 2.0;
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

    // 唤醒队列
    audioPacketQueue_.clear();
    videoPacketQueue_.clear();
    frameQueue_.clear();
    cmdQueue_.clear();

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
}