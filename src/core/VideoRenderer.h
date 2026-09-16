#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

#include <QImage>
#include <chrono>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

/**
 * @brief 视频渲染器
 *
 * 使用 libswscale 将解码后的 AVFrame（YUV 等格式）转换为 RGBA QImage，
 * 并在同一步骤中缩放到显示尺寸（避免"先按原始分辨率转换、再由 UI 软件缩放"
 * 的双重全分辨率缩放开销）。
 * 自动检测输入尺寸/格式变化并重建转换上下文。
 *
 * 线程约束：所有方法只在创建线程（主线程渲染定时器）调用。
 *
 * 目标缓冲按 libavutil 的对齐契约（av_image_*，align=64）分配：
 * libswscale 的 SIMD 例程在行尾/最后一行可能过量写入至多一个向量宽度，
 * 对齐 stride 自带行尾 slack，这是 ffplay/mpv 的标准做法，而非按实测字节数
 * 加宽 QImage 的魔数式修补。
 */
class VideoRenderer {
public:
    VideoRenderer()  = default;
    ~VideoRenderer() { close(); }

    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    /**
     * @brief 初始化 sws 上下文（输出尺寸默认为源尺寸）
     * @return 成功返回 true
     */
    bool open(int srcW, int srcH, AVPixelFormat srcFmt);

    /**
     * @brief 请求输出（显示）尺寸
     *
     * 不立即重建 sws（拖动窗口会产生大量连续尺寸），而是记录 pending，
     * 在 render() 中停顿超过防抖阈值后一次性重建；期间旧上下文继续服务，
     * UI 层的 drawImage 缩放兜底。
     * 传入非正值时回退到源尺寸。
     */
    void setTargetSize(int w, int h);

    /**
     * @brief 将 AVFrame 渲染为 QImage
     * @return 缩放/转换后的 QImage（深拷贝，调用方可安全持有）
     */
    QImage render(AVFrame* frame);

    /** @brief 关闭并释放 sws 上下文与目标缓冲 */
    void close();

    /** @return 当前源宽度 */
    int srcWidth()  const { return srcW_; }
    int srcHeight() const { return srcH_; }

    /** @return 当前输出宽度 */
    int dstWidth()  const { return dstW_; }
    int dstHeight() const { return dstH_; }

private:
    /**
     * @brief 用候选参数局部构建上下文，成功才提交成员（失败可自愈）
     * @return 成功返回 true；失败时所有成员保持原状
     */
    bool buildContext(int srcW, int srcH, AVPixelFormat srcFmt,
                      int dstW, int dstH);

    /** @brief 必要时应用防抖后的 pending 目标尺寸 */
    void applyPendingTarget();

    /** @brief 按 (w,h) 分配/复用对齐目标缓冲 */
    bool ensureDstBuffer(int w, int h);

    static constexpr int kAlign = 64;

    SwsContext*  swsCtx_ = nullptr;
    int          srcW_   = 0;
    int          srcH_   = 0;
    AVPixelFormat srcFmt_ = AV_PIX_FMT_NONE;
    int          dstW_   = 0;   // 输出宽度
    int          dstH_   = 0;   // 输出高度

    // 对齐目标缓冲（av_malloc 分配，仅尺寸变化时重分配）
    uint8_t* dstBuf_       = nullptr;
    uint8_t* dstData_[4]   = {};
    int      dstLinesize_[4] = {};
    int      bufW_ = 0, bufH_ = 0;

    // 防抖的目标尺寸请求
    int  pendingW_ = 0, pendingH_ = 0;
    bool pendingValid_ = false;
    std::chrono::steady_clock::time_point pendingTp_{};
    static constexpr int kResizeDebounceMs = 60;
};

#endif // VIDEO_RENDERER_H
