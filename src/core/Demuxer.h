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
     * @return 堆分配的 AVPacket（调用方需 av_packet_unref + delete），
     *         EOF 或出错时返回 nullptr
     */
    AVPacket* read();

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