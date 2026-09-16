#ifndef DEMUXER_H
#define DEMUXER_H

#include <string>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

/**
 * @brief 解复用模块
 *
 * 封装 AVFormatContext 生命周期与流信息解析。
 * 职责：
 *  - 打开媒体文件/URL
 *  - 探测流信息（音视频流索引、时长、time_base）
 *  - 循环读取 AVPacket
 */
/** @brief 单次 packet 读取结果（区分 EOF 与读错误） */
enum class ReadStatus {
    Ok,    ///< 成功，pkt 输出堆分配的 AVPacket（调用方 av_packet_free）
    Eof,   ///< 已到达文件末尾（seek 后可再次读取）
    Error  ///< 读取发生错误
};

class Demuxer {
public:
    Demuxer();
    ~Demuxer();

    // 禁止拷贝
    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    /**
     * @brief 打开媒体文件或网络流
     * @param url 文件路径或 URL
     * @return 成功返回 true
     */
    bool open(const std::string& url);

    /**
     * @brief 读取下一个 AVPacket
     * @param pkt[out] Ok 时输出堆分配的 AVPacket（调用方负责 av_packet_free）
     * @return Ok / Eof / Error；Eof/Error 时 pkt 保持为 nullptr
     */
    ReadStatus readPacket(AVPacket*& pkt);

    /** @brief 关闭并释放资源 */
    void close();

    // ── 流信息 ──
    int             audioIndex()      const { return audioIndex_; }
    int             videoIndex()      const { return videoIndex_; }
    double          durationSec()     const;
    AVRational      audioTimeBase()   const;
    AVRational      videoTimeBase()   const;
    AVFormatContext* formatCtx()       const { return fmtCtx_; }

    /** @brief 元数据（艺术家、标题等） */
    const AVFormatContext* operator->() const { return fmtCtx_; }

private:
    AVFormatContext* fmtCtx_ = nullptr;
    int audioIndex_ = -1;
    int videoIndex_ = -1;

    /** @brief 查找最佳音视频流索引 */
    void findStreamIndices();
};

#endif // DEMUXER_H