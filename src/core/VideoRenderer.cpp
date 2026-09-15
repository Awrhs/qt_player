#include "VideoRenderer.h"
#include "../utils/Log.h"

LOG_TAG("VideoRenderer")

bool VideoRenderer::open(int srcW, int srcH, AVPixelFormat srcFmt) {
    close();

    swsCtx_ = sws_getContext(srcW, srcH, srcFmt,
                              srcW, srcH, AV_PIX_FMT_RGBA,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) {
        LOG_E("sws_getContext failed (%dx%d, fmt=%d)", srcW, srcH, srcFmt);
        return false;
    }

    srcW_   = srcW;
    srcH_   = srcH;
    srcFmt_ = srcFmt;

    LOG_I("VideoRenderer initialized: %dx%d, fmt=%d", srcW, srcH, srcFmt);
    return true;
}

QImage VideoRenderer::render(AVFrame* frame) {
    if (!swsCtx_) return {};

    // 检测分辨率/格式变化，自动重建
    if (frame->width != srcW_ || frame->height != srcH_ || frame->format != srcFmt_) {
        LOG_I("Video parameters changed: %dx%d fmt=%d -> %dx%d fmt=%d",
              srcW_, srcH_, srcFmt_, frame->width, frame->height, frame->format);
        if (!open(frame->width, frame->height, (AVPixelFormat)frame->format)) {
            return {};
        }
    }

    // 分配 RGBA 缓冲
    QImage image(frame->width, frame->height, QImage::Format_RGBA8888);

    uint8_t* dstData[1]     = { image.bits() };
    int      dstStride[1]   = { static_cast<int>(image.bytesPerLine()) };

    int ret = sws_scale(swsCtx_,
                        frame->data, frame->linesize, 0, frame->height,
                        dstData, dstStride);
    if (ret < 0) {
        LOG_E("sws_scale failed: %d", ret);
        return {};
    }

    return image;
}

void VideoRenderer::close() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    srcW_ = srcH_ = 0;
    srcFmt_ = AV_PIX_FMT_NONE;
}