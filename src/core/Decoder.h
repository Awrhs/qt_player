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
 * 音频/视频各创建一个实例，且每个实例只由其解码线程使用
 * （seek 的 flush 由解码线程在看到包代际变化时自行执行，无需跨线程加锁）。
 *
 * 硬解路径：
 *   open(useHW) 时设置 get_format 回调 → receiveFrame 自动回读到系统内存
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
     * @param timeBase 所属流的 time_base（设置 pkt_timebase，供解码器正确处理时长/裁剪）
     * @param useHW    是否尝试硬件解码（仅视频有效）
     * @return 成功返回 true；useHW 时若硬解不可用则自动回退软解
     */
    bool open(const AVCodecParameters* codecpar, AVRational timeBase, bool useHW = false);

    /**
     * @brief 提交一个 AVPacket 到解码器
     * @param pkt 非空表示解码数据；NULL 表示进入排空（drain）模式
     * @return avcodec_send_packet 的返回值
     */
    int sendPacket(const AVPacket* pkt);

    /**
     * @brief 接收解码后的 AVFrame
     * @return 0 成功；AVERROR(EAGAIN) 需要更多包；AVERROR_EOF 排空完成
     */
    int receiveFrame(AVFrame* frame);

    /** @brief 清空解码器内部缓冲（seek / drain 之后调用） */
    void flush();

    /** @brief 关闭并释放所有资源 */
    void close();

    AVCodecContext* ctx() const { return ctx_; }
    bool isOpen() const { return ctx_ != nullptr; }
    bool isHardware() const { return hwDeviceCtx_ != nullptr; }

private:
    AVCodecContext*  ctx_         = nullptr;
    AVBufferRef*     hwDeviceCtx_ = nullptr;   // DXVA2 device
    AVPixelFormat    hwPixFmt_    = AV_PIX_FMT_NONE;
    bool             useHW_       = false;

    bool tryInitHardware(const AVCodec* codec);
    static AVPixelFormat getHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);
};

#endif // DECODER_H
