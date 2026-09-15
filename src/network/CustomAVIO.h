#ifndef CUSTOM_AVIO_H
#define CUSTOM_AVIO_H

#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

class RingBuffer;

/**
 * @brief 自定义 AVIOContext（网络滑动窗口缓存）
 *
 * 使用 RingBuffer 为 FFmpeg 提供数据，使 avformat_open_input
 * 可以从内存中读取而无需直接访问网络文件。
 *
 * 使用方式：
 *   auto avio = CustomAVIO::createForUrl(ring, url);
 *   fmtCtx->pb = avio;
 *   avformat_open_input(&fmtCtx, "", ...); // 传入空 URL
 */
class CustomAVIO {
public:
    /**
     * @brief 创建自定义 AVIOContext
     * @param ring 环形缓冲指针
     * @param url  原始 URL（用于日志）
     * @return 分配的 AVIOContext，使用后需 avio_context_free
     */
    static AVIOContext* create(RingBuffer* ring, const std::string& url);

    /** @brief 销毁 AVIOContext */
    static void destroy(AVIOContext* ctx);

private:
    /**
     * @brief AVIO read_packet 回调
     * @param opaque RingBuffer 指针
     * @param buf    目标缓冲
     * @param bufSize 请求大小
     * @return 读取字节数，0=EOF，<0=错误
     */
    static int readPacket(void* opaque, uint8_t* buf, int bufSize);

    /**
     * @brief AVIO seek 回调
     * @param opaque  RingBuffer 指针
     * @param offset  偏移
     * @param whence  SEEK_SET/SEEK_CUR/SEEK_END/AVSEEK_SIZE
     * @return 新位置或 -1
     */
    static int64_t seek(void* opaque, int64_t offset, int whence);
};

#endif // CUSTOM_AVIO_H