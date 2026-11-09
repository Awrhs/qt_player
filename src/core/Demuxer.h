#ifndef DEMUXER_H
#define DEMUXER_H

#include <string>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

/** @brief 单次 packet 读取结果 */
enum class ReadStatus {
    Ok,     ///< 成功，pkt 输出堆分配的 AVPacket（调用方 av_packet_free）
    Again,  ///< 暂时无数据（网络流），稍后重试
    Eof,    ///< 已到达文件末尾（seek 后可再次读取）
    Error   ///< 读取发生错误
};

/**
 * @brief 解复用模块
 *
 * 封装 AVFormatContext 生命周期与流信息解析：
 *  - 打开媒体文件/URL（支持中断回调，关闭时可打断阻塞的网络 I/O）
 *  - 选择最佳音视频流（av_find_best_stream）
 *  - 读包、seek、start_time / duration 等信息
 */
class Demuxer {
public:
    Demuxer();
    ~Demuxer();

    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    /**
     * @brief 设置中断标志：*abort 为 true 时打断正在进行的 open/read（须在 open 前设置）
     */
    void setAbortFlag(const std::atomic<bool>* abort) { abort_ = abort; }

    bool open(const std::string& url);

    /**
     * @brief 读取下一个 AVPacket
     * @param pkt[out] Ok 时输出堆分配的 AVPacket（调用方负责 av_packet_free）
     */
    ReadStatus readPacket(AVPacket*& pkt);

    /**
     * @brief 定位到指定流时间（秒，含 start_time），落到之前最近的关键帧
     *
     * 只对默认流做一次 av_seek_frame（AV_TIME_BASE 单位），由解复用器同步其它流；
     * 对每条流分别 seek 会互相覆盖，在 MKV 等格式上甚至导致 seek 失败。
     */
    bool seek(double targetSec);

    void close();

    // ── 流信息 ──
    int              audioIndex()    const { return audioIndex_; }
    int              videoIndex()    const { return videoIndex_; }
    /** @brief 时长（秒）；容器未知时取各流最大时长，仍未知返回 0 */
    double           durationSec()   const;
    /** @brief 起始时间（秒）；未知返回 0 */
    double           startTimeSec()  const;
    AVRational       audioTimeBase() const;
    AVRational       videoTimeBase() const;
    /** @brief 视频帧率（avg_frame_rate 或 r_frame_rate），未知返回 0 */
    double           videoFrameRate() const;
    /** @brief 视频流是否只是封面图（附加图片） */
    bool             videoIsAttachedPic() const;
    bool             isSeekable() const;
    AVFormatContext* formatCtx()     const { return fmtCtx_; }

private:
    static int interruptCallback(void* opaque);
    void findStreamIndices();

    AVFormatContext* fmtCtx_ = nullptr;
    const std::atomic<bool>* abort_ = nullptr;
    int audioIndex_ = -1;
    int videoIndex_ = -1;
};

#endif // DEMUXER_H
