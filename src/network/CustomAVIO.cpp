#include "CustomAVIO.h"
#include "RingBuffer.h"
#include "../utils/Log.h"

#include <cstring>

LOG_TAG("CustomAVIO")

AVIOContext* CustomAVIO::create(RingBuffer* ring, const std::string& url) {
    constexpr int BUFFER_SIZE = 32768; // 32KB internal buffer

    uint8_t* buffer = (uint8_t*)av_malloc(BUFFER_SIZE);
    if (!buffer) return nullptr;

    AVIOContext* ctx = avio_alloc_context(
        buffer, BUFFER_SIZE,
        0,                // write flag = 0 (read-only)
        ring,            // opaque
        readPacket,
        nullptr,         // write callback
        seek             // seek callback
    );

    if (!ctx) {
        av_free(buffer);
        return nullptr;
    }

    LOG_I("CustomAVIO created, ring=%p, buffer=%dKB", (void*)ring, BUFFER_SIZE/1024);
    return ctx;
}

void CustomAVIO::destroy(AVIOContext* ctx) {
    if (ctx) {
        uint8_t* buffer = ctx->buffer;
        avio_context_free(&ctx);
        av_free(buffer);
    }
}

int CustomAVIO::readPacket(void* opaque, uint8_t* buf, int bufSize) {
    RingBuffer* ring = static_cast<RingBuffer*>(opaque);
    if (!ring) return -1;

    // 等待数据
    size_t avail = ring->waitForData(1, 5000);
    if (avail == 0) return AVERROR_EOF;

    size_t toRead = std::min(static_cast<size_t>(bufSize), avail);
    size_t read = ring->read(buf, toRead);
    return static_cast<int>(read);
}

int64_t CustomAVIO::seek(void* opaque, int64_t offset, int whence) {
    RingBuffer* ring = static_cast<RingBuffer*>(opaque);
    if (!ring) return -1;

    Q_UNUSED(ring)
    Q_UNUSED(offset)

    // 简化实现：不支持随机 seek（网络流仅支持顺序读取）
    // 对于点播场景需要回源 seek，这里暂不支持
    if (whence == AVSEEK_SIZE) {
        return -1; // 不知道总大小
    }

    return -1; // 不支持 seek
}