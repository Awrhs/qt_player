#ifndef DECODER_H
#define DECODER_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

/**
 * @brief 解码器模块
 *
 * 封装单个 AVCodecContext 的生命周期，支持软解和 DXVA2 硬解。
 * 音频/视频各创建一个实例。
 *
 * 硬解路径：
 *   setHWDevice() → open() 时设置 get_format 回调 → receiveFrame 自动回读到系统内存
 */
class Decoder {
public:
    Decoder();
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /**
     * @brief 打开解码器
     * @param codecpar 从 AVStream 获取的编码参数
     * @param useHW    是否尝试硬件解码（仅视频有效）
     * @return 成功返回 true；useHW 时若硬解不可用则自动回退软解
     */
    bool open(const AVCodecParameters* codecpar, bool useHW = false);

    /**
     * @brief 提交一个 AVPacket 到解码器
     * @param pkt 非空表示解码数据；NULL 表示冲刷（flush）
     * @return avcodec_send_packet 的返回值
     */
    int sendPacket(AVPacket* pkt);

    /**
     * @brief 接收解码后的 AVFrame
     * @param frame 预分配的帧（av_frame_alloc）
     * @return avcodec_receive_frame 的返回值：
     *         0 成功；EAGAIN 需要更多包；EOF/AVERROR_EOF 冲刷完成
     */
    int receiveFrame(AVFrame* frame);

    /** @brief 清空解码器内部缓冲（seek 时调用） */
    void flush();

    /** @brief 关闭并释放所有资源 */
    void close();

    /** @return 解码器上下文，用于外部获取参数 */
    AVCodecContext* ctx() const { return ctx_; }

    /** @return 是否当前使用了硬件解码 */
    bool isHardware() const { return hwDeviceCtx_ != nullptr; }

private:
    AVCodecContext*  ctx_         = nullptr;
    AVBufferRef*     hwDeviceCtx_ = nullptr;   // DXVA2 device
    AVPixelFormat    hwPixFmt_    = AV_PIX_FMT_NONE;
    bool             useHW_       = false;

    /**
     * @brief 尝试创建 DXVA2 硬件设备并配置解码器
     * @return 成功返回 true
     */
    bool tryInitHardware(const AVCodec* codec);

    /**
     * @brief FFmpeg get_format 回调（C 函数）
     * @param ctx  AVCodecContext
     * @param pix_fmts 支持的像素格式列表
     * @return 选择的像素格式
     */
    static AVPixelFormat getHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);
};

#endif // DECODER_H