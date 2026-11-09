#ifndef PLAYER_CONTROLLER_H
#define PLAYER_CONTROLLER_H

#include <QObject>
#include <QImage>
#include <QTimer>
#include <QString>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "BoundedQueue.h"
#include "PacketQueue.h"
#include "Demuxer.h"
#include "Decoder.h"
#include "VideoRenderer.h"
#include "MediaClock.h"
#include "AudioOutput.h"
#include "Equalizer.h"
#include "SpectrumAnalyzer.h"

struct AVFrame;

/**
 * @brief 播放器状态
 */
enum class PlayState {
    Idle,     ///< 未打开文件
    Opened,   ///< （保留）
    Playing,
    Paused,   ///< 用户暂停或自然播放结束（见 isEnded()）
    Seeking,  ///< （保留）
    Stopped   ///< 已停止：管线释放，需重新 open
};

/** @brief 解码线程 → 主线程的视频帧（带 seek 代际） */
struct VideoFrame {
    AVFrame* frame  = nullptr;
    uint64_t serial = 0;
};

/**
 * @brief 播放控制器
 *
 * 线程模型：
 *  - 解复用线程：av_read_frame → 音/视频包队列（PacketQueue，带 serial）；处理 seek
 *  - 音频解码线程：包 → 解码 → AudioOutput（变速 + PCM 队列 + 主时钟）
 *  - 视频解码线程：包 → 解码 → seek pre-roll 过滤 → 帧队列
 *  - 主线程渲染定时器：按主时钟取帧上屏（提前等待、落后丢帧）、结束判定、循环
 *
 * seek 采用 ffplay 风格的代际（serial）机制：seek 时递增 serial 并清空队列，
 * 解码线程看到 serial 变化自行 flush 解码器，旧代际的包/帧/PCM 一律丢弃。
 * 位置一律以“相对媒体起点”的秒数对外暴露（内部换算容器 start_time）。
 */
class PlayerController : public QObject {
    Q_OBJECT
public:
    explicit PlayerController(QObject* parent = nullptr);
    ~PlayerController() override;

    // ── 播放控制（均在主线程调用） ──
    void open(const QString& path);
    void open(const QString& path, bool useHardwareDecode);
    void play();
    void pause();
    void togglePlayPause();
    /** @brief 停止并释放管线（再次播放需 open） */
    void stop();
    /** @brief 同步关闭（窗口 closeEvent 调用）：停设备 + join 全部工作线程 */
    void shutdown();

    /** @brief 定位到相对起点的秒数；暂停态下保持暂停并显示目标帧 */
    void seek(double seconds);
    void seekRelative(double deltaSeconds);
    /** @brief 倍速 0.5~2.0（音频保持音调） */
    void setSpeed(double factor);
    /** @brief 逐帧前进（播放中先暂停） */
    void stepForward();
    /** @brief 逐帧后退（播放中先暂停） */
    void stepBackward();
    void setVolume(int percent);
    void setMuted(bool muted);
    /** @brief 循环播放：到达末尾后从头继续 */
    void setLoop(bool enabled);
    /** @brief 视频输出尺寸（显示控件按窗口大小与宽高比计算后下发） */
    void setVideoTargetSize(int w, int h);

    // ── 状态查询 ──
    PlayState state()    const { return state_.load(); }
    /** @brief 当前位置（相对起点，秒） */
    double    position() const;
    double    duration() const { return duration_; }
    int       videoWidth()  const;
    int       videoHeight() const;
    bool      hasVideo() const { return hasVideo_; }
    bool      hasAudio() const { return hasAudio_; }
    double    speed()    const { return speed_.load(); }
    int       volume()   const { return volume_; }
    bool      muted()    const { return muted_; }
    bool      loop()     const { return loop_.load(); }
    /** @brief 是否处于自然播放结束后的末帧暂停态 */
    bool      isEnded()  const { return ended_; }
    /** @brief 最近上屏帧的时间（相对起点，秒） */
    double    currentFramePts() const { return lastFrameUiPts_; }
    QString   currentPath() const { return path_; }
    int       droppedFrames() const { return droppedTotal_; }
    bool      audioUsingNullSink() const;

    void setHardwareDecode(bool enabled) { hwDecodeEnabled_ = enabled; }
    bool hwDecodeEnabled() const { return hwDecodeEnabled_; }

    Equalizer*        equalizer()        { return equalizer_.get(); }
    SpectrumAnalyzer* spectrumAnalyzer() { return spectrumAnalyzer_.get(); }

signals:
    void stateChanged(PlayState newState);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void firstFrameRendered();
    void errorOccurred(const QString& message);
    /** @brief 非致命问题（状态栏提示即可） */
    void warning(const QString& message);
    void speedChanged(double factor);
    void loopChanged(bool enabled);
    void frameReady(const QImage& image);
    /** @brief 一帧已上屏（相对起点的秒数） */
    void frameDisplayed(double seconds);
    /** @brief 自然播放结束（已进入末帧暂停态；循环模式下不会发出） */
    void playbackFinished();
    /** @brief 显示宽高比变化（含 SAR 修正）；<0 表示回退到像素比 */
    void displayAspectChanged(double aspect);
    void mediaOpened(const QString& path, bool hasVideo, bool hasAudio);

private slots:
    void onRenderTick();

private:
    // 线程函数
    void demuxLoop();
    void audioDecodeLoop();
    void videoDecodeLoop();

    // 管线生命周期
    void startThreads();
    void closeMedia();
    void clearFrameQueue();
    static void releaseFrame(VideoFrame& vf);

    // seek
    void requestSeek(double targetStreamSec);
    bool takePendingSeek(double& target);
    void waitIdle(int ms);
    void processSeek(double targetStreamSec);
    bool queuesHaveEnough() const;

    // 渲染 / 同步（主线程）
    void scheduleRender(int ms);
    bool popFrame(VideoFrame& out, int waitMs);
    void displayFrame(VideoFrame& vf);
    double frameTime(const AVFrame* f) const;
    bool audioDrivesClock() const;
    bool checkEndOfStream();
    void enterPlaybackFinished();
    void resumePlayback();
    void updateDisplayAspect(AVFrame* frame);

    // ── 状态 ──
    std::atomic<PlayState> state_{PlayState::Idle};
    std::atomic<double>    speed_{1.0};
    std::atomic<bool>      loop_{false};
    int   volume_ = 100;
    bool  muted_  = false;
    bool  hwDecodeEnabled_ = false;
    QString path_;

    bool   hasVideo_  = false;
    bool   hasAudio_  = false;
    double startTime_ = 0.0;        // 容器起始时间（秒）
    double duration_  = 0.0;        // 时长（秒）
    AVRational videoTb_{0, 1};
    AVRational audioTb_{0, 1};
    double defaultFrameDur_ = 0.04;

    // ── 核心模块 ──
    std::unique_ptr<Demuxer>          demuxer_;
    std::unique_ptr<Decoder>          videoDecoder_;
    std::unique_ptr<Decoder>          audioDecoder_;
    std::unique_ptr<VideoRenderer>    videoRenderer_;
    std::unique_ptr<AudioOutput>      audioOutput_;
    std::unique_ptr<Equalizer>        equalizer_;
    std::unique_ptr<SpectrumAnalyzer> spectrumAnalyzer_;
    MediaClock                        clock_;

    // ── 队列 ──
    PacketQueue              audioPackets_;
    PacketQueue              videoPackets_;
    BoundedQueue<VideoFrame> frameQueue_;

    // ── 线程 ──
    std::thread       demuxThread_;
    std::thread       audioDecodeThread_;
    std::thread       videoDecodeThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> abortIo_{false};

    // seek 请求（主线程 → 解复用线程）
    std::mutex              seekMtx_;
    std::condition_variable seekCv_;
    bool                    seekPending_ = false;
    double                  seekTarget_  = 0.0;
    std::atomic<bool>       seekPendingFlag_{false};
    std::atomic<double>     pendingSeekUi_{0.0};

    // 代际与 EOF
    std::atomic<uint64_t> serial_{0};
    std::atomic<uint64_t> audioEosSerial_{0};
    std::atomic<uint64_t> videoEosSerial_{0};
    std::atomic<bool>     demuxEof_{false};
    std::atomic<bool>     showFrameWhenPaused_{false};

    // ── 渲染（仅主线程） ──
    QTimer*    renderTimer_ = nullptr;
    VideoFrame pendingFrame_;              // 提前到达、等待展示窗口的帧
    bool       firstFrameShown_   = false;
    bool       audioStartPending_ = false; // 有视频时音频设备延迟到首帧就绪才启动
    std::chrono::steady_clock::time_point playStartTp_{};
    double     lastFrameStreamPts_ = 0.0;
    double     lastFrameDur_       = 0.0;
    double     lastFrameUiPts_     = 0.0;
    bool       stepped_            = false; // 暂停态逐帧过，恢复播放前需把音频对齐到画面
    bool       ended_              = false;
    bool       eofSettling_        = false;
    std::chrono::steady_clock::time_point eofSettleTp_{};
    std::chrono::steady_clock::time_point drainedSince_{};
    uint64_t   loopSeekSerial_     = 0;
    double     currentAspect_      = -1.0;
    int        statDropped_        = 0;
    int        droppedTotal_       = 0;
    std::chrono::steady_clock::time_point lastStatTp_{};
};

#endif // PLAYER_CONTROLLER_H
