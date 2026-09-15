#ifndef PLAYER_CONTROLLER_H
#define PLAYER_CONTROLLER_H

#include <QObject>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
#include <atomic>
#include <thread>

#include "BoundedQueue.h"
#include "Demuxer.h"
#include "Decoder.h"
#include "VideoRenderer.h"
#include "AudioClock.h"
#include "AudioOutput.h"
#include "Equalizer.h"
#include "SpectrumAnalyzer.h"

struct AVFrame;

/**
 * @brief 播放器状态枚举
 */
enum class PlayState {
    Idle,
    Opened,
    Playing,
    Paused,
    Seeking,
    Stopped
};

/**
 * @brief 播放命令类型
 */
enum class CommandType {
    Play,
    Pause,
    Stop,
    Seek,
    SetSpeed,
    StepForward,
    StepBackward,
    Quit
};

struct Command {
    CommandType type;
    double      arg = 0.0;
};

/**
 * @brief 播放控制器（M2 完整版）
 *
 * 管理：
 *  - 解复用线程：av_read_frame → 音频包队列 + 视频包队列
 *  - 音频解码线程：音频包 → 解码 → 重采样 → AudioOutput
 *  - 视频解码线程：视频包 → 解码 → 帧队列
 *  - 渲染定时器：帧队列 → 音频主时钟同步 → sws_scale → emit frameReady
 */
class PlayerController : public QObject {
    Q_OBJECT
public:
    explicit PlayerController(QObject* parent = nullptr);
    ~PlayerController() override;

    // ── 播放控制 ──
    void open(const QString& path);
    void open(const QString& path, bool useHardwareDecode);
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    void setSpeed(double factor);
    void stepForward();
    void stepBackward();

    // ── 状态查询 ──
    PlayState state()    const { return state_; }
    double    position() const;
    double    duration() const;
    double    speed()    const { return speed_; }
    void setHardwareDecode(bool enabled) { hwDecodeEnabled_ = enabled; }
    bool hwDecodeEnabled() const { return hwDecodeEnabled_; }

    // ── EQ & Spectrum ──
    Equalizer*        equalizer()       { return equalizer_.get(); }
    SpectrumAnalyzer* spectrumAnalyzer() { return spectrumAnalyzer_.get(); }

signals:
    void stateChanged(PlayState newState);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void firstFrameRendered();
    void errorOccurred(const QString& message);
    void speedChanged(double factor);
    void frameReady(const QImage& image);

private slots:
    void onRenderTick();

private:
    // ── 线程函数 ──
    void demuxLoop();
    void audioDecodeLoop();
    void videoDecodeLoop();

    // ── 内部工具 ──
    void postCommand(Command cmd);
    void processCommand(const Command& cmd);
    void startThreads();
    void stopThreads();
    void clearQueues();

    // 同步
    double calcVideoDelay(AVFrame* frame);

    // ── 状态 ──
    std::atomic<PlayState> state_{PlayState::Idle};
    double speed_ = 1.0;
    bool   hwDecodeEnabled_ = false;

    // ── 核心模块 ──
    std::unique_ptr<Demuxer>        demuxer_;
    std::unique_ptr<Decoder>        videoDecoder_;
    std::unique_ptr<Decoder>        audioDecoder_;
    std::unique_ptr<VideoRenderer>  videoRenderer_;
    std::unique_ptr<AudioOutput>    audioOutput_;
    std::unique_ptr<Equalizer>      equalizer_;
    std::unique_ptr<SpectrumAnalyzer> spectrumAnalyzer_;
    AudioClock                      audioClock_;
    ExternalClock                   externalClock_;

    // ── 队列 ──
    BoundedQueue<AVPacket*>         audioPacketQueue_{100};
    BoundedQueue<AVPacket*>         videoPacketQueue_{100};
    BoundedQueue<AVFrame*>          frameQueue_{12};   // 解码 → 渲染
    BoundedQueue<Command>           cmdQueue_{16};

    // ── 线程 ──
    std::thread                     demuxThread_;
    std::thread                     audioDecodeThread_;
    std::thread                     videoDecodeThread_;
    std::atomic<bool>               threadsRunning_{false};

    // ── 渲染 ──
    QTimer*                         renderTimer_ = nullptr;
    int                             frameCount_ = 0;
};

#endif // PLAYER_CONTROLLER_H