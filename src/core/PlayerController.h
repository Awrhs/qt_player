#ifndef PLAYER_CONTROLLER_H
#define PLAYER_CONTROLLER_H

#include <QObject>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>

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

    /**
     * @brief 同步关闭（仅主线程，窗口 closeEvent 调用）
     *
     * 与 stop()（投递异步命令、线程保留供 reopen）不同：立即在主线程停止
     * 音频设备并 join 全部工作线程。必须在主线程同步执行——若只投递排队
     * 事件停音频，closeEvent 返回后事件循环可能已停止，事件永不执行，
     * 会导致阻塞在 PCM 入队上的音频解码线程无法 join（关窗死锁）。
     */
    void shutdown();

    void seek(double seconds);
    void setSpeed(double factor);
    void stepForward();
    void stepBackward();
    void setVolume(int percent);
    void setMuted(bool muted);

    /**
     * @brief 设置视频输出尺寸（显示控件按窗口大小与宽高比计算后下发）
     *
     * sws 将直接缩放到该尺寸，避免 UI 线程再做一次全分辨率软件缩放。
     * 主线程调用。
     */
    void setVideoTargetSize(int w, int h);

    // ── 状态查询 ──
    PlayState state()    const { return state_; }
    double    position() const;
    double    duration() const;
    /** @brief 视频源尺寸（打开成功后有效，否则返回 0） */
    int       videoWidth() const;
    int       videoHeight() const;
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
    /** @brief 自然播放结束（已进入末帧暂停态） */
    void playbackFinished();
    /** @brief 显示宽高比变化（含 SAR 修正）；<0 表示回退到像素比 */
    void displayAspectChanged(double aspect);

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
    void resetEofState();

    // 同步
    double calcVideoDelay(AVFrame* frame);

    // seek pre-roll：返回 true 表示该音频帧仍早于目标点，必须丢弃
    bool handleAudioPreroll(AVFrame* frame, double& lastDiscardTarget,
                            int& prerollDiscarded);

    // EOF 结束判定与切入（均在主线程）
    bool checkPlaybackFinished();
    void enterPlaybackFinished();
    // 根据帧的 SAR 发射 displayAspectChanged（主线程）
    void updateDisplayAspect(AVFrame* frame);

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

    // 暂存被"超前"检查挡下的帧，下一 tick 重试
    // 主线程 onRenderTick 持有/消费；clearQueues() 可能在解复用线程（seek/stop）
    // 释放它，因此用互斥锁保护
    std::mutex                      pendingFrameMtx_;
    AVFrame*                        pendingEarlyFrame_ = nullptr;

    // 丢帧统计（每秒限频日志）
    int                             statDroppedLate_ = 0;
    std::chrono::steady_clock::time_point lastStatTime_{};

    // 打开/seek 后首个渲染帧到达前为 true：首帧到达时把外部时钟锚定到该帧 PTS，
    // 避免解码预热期间时钟空跑导致开头一批帧被判为"落后"而丢弃（仅视频-only 生效）
    bool                            clockAnchorPending_ = false;

    // 音视频预滚：有视频流时音频设备延迟到首帧渲染才 start
    bool                            audioStartPending_ = false;
    // audioStartPending_ 的挂起时间戳（主线程），超时 2s 强制启动音频
    std::chrono::steady_clock::time_point audioStartTp_{};

    // ── EOF / 结束状态 ──
    // demux 读到 AVERROR_EOF（demux 线程写、主线程读）
    // 哨兵 nullptr 消费完毕、解码器与 swr 尾部已排空（对应解码线程写）
    // seek/open/stop 时统一复位
    std::atomic<bool> demuxEof_{false};
    std::atomic<bool> audioDrainEof_{false};
    std::atomic<bool> videoDrainEof_{false};
    // 已切入“末帧暂停”（区别于用户手动暂停，决定 play() 是否从头重播）
    std::atomic<bool> ended_{false};
    //  drain 完成后等待设备把最后缓冲真正播完的稳态计时（主线程）
    bool                            eofSettling_ = false;
    std::chrono::steady_clock::time_point eofSettleTp_{};
    // 每次 seek 自增：解码线程据此确认 drain 期间没有发生 seek（避免
    // drain 完成写 eof 标志覆盖掉 seek 的复位）
    std::atomic<uint64_t>           seekGeneration_{0};

    // 当前显示宽高比（含 SAR，仅主线程访问）；<0 表示尚未感知
    double                          currentAspect_ = -1.0;

    // seek pre-roll：av_seek_frame 落到目标点之前的关键帧，解码出的
    // 音视频帧时间戳早于目标点，必须跳过而不是播放。
    //  - 音频：解码线程据此丢弃整帧 PCM（>=0 表示丢弃阈值秒）
    //  - 视频：追赶期间的丢帧属于预期行为，静默处理不计入 late 统计；
    //    直到第一帧进入可显示窗口才解除
    std::atomic<double>             audioDiscardUntilSec_{-1.0};
    std::atomic<bool>               videoPreroll_{false};
};

#endif // PLAYER_CONTROLLER_H