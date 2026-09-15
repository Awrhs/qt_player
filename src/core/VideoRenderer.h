#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

#include <QImage>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

/**
 * @brief 视频渲染器
 *
 * 使用 libswscale 将解码后的 AVFrame（YUV 等格式）转换为 RGBA QImage。
 * 自动检测输入尺寸/格式变化并重建转换上下文。
 */
class VideoRenderer {
public:
    VideoRenderer()  = default;
    ~VideoRenderer() { close(); }

    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    /**
     * @brief 初始化/重建 sws 上下文
     * @param srcW   源宽度
     * @param srcH   源高度
     * @param srcFmt 源像素格式
     * @return 成功返回 true
     */
    bool open(int srcW, int srcH, AVPixelFormat srcFmt);

    /**
     * @brief 将 AVFrame 渲染为 QImage
     * @param frame 解码后的视频帧（系统内存格式，非硬件帧）
     * @return 转换后的 QImage（深拷贝，调用方可安全持有）
     */
    QImage render(AVFrame* frame);

    /** @brief 关闭并释放 sws 上下文 */
    void close();

    /** @return 当前源宽度 */
    int srcWidth()  const { return srcW_; }
    int srcHeight() const { return srcH_; }

private:
    SwsContext*  swsCtx_ = nullptr;
    int          srcW_   = 0;
    int          srcH_   = 0;
    AVPixelFormat srcFmt_ = AV_PIX_FMT_NONE;
};

#endif // VIDEO_RENDERER_H