#include "VideoRenderer.h"
#include "../utils/Log.h"

#include <algorithm>

LOG_TAG("VideoRenderer")

bool VideoRenderer::buildContext(int srcW, int srcH, AVPixelFormat srcFmt,
                                 int dstW, int dstH) {
    if (srcW <= 0 || srcH <= 0 || srcFmt == AV_PIX_FMT_NONE ||
        dstW <= 0 || dstH <= 0) {
        return false;
    }

    // 局部构建，成功才提交成员：失败时旧上下文原样保留，后续帧/resize
    // 会再次尝试，避免“成员已改、context 仍旧”的永不自愈状态
    SwsContext* ctx = sws_getContext(srcW, srcH, srcFmt,
                                     dstW, dstH, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!ctx) {
        LOG_E("sws_getContext failed (%dx%d -> %dx%d, fmt=%d)",
              srcW, srcH, dstW, dstH, static_cast<int>(srcFmt));
        return false;
    }

    sws_freeContext(swsCtx_);
    swsCtx_ = ctx;
    srcW_ = srcW;
    srcH_ = srcH;
    srcFmt_ = srcFmt;
    dstW_ = dstW;
    dstH_ = dstH;
    LOG_I("sws context %dx%d fmt=%d -> %dx%d", srcW, srcH,
          static_cast<int>(srcFmt), dstW, dstH);
    return true;
}

bool VideoRenderer::open(int srcW, int srcH, AVPixelFormat srcFmt) {
    close();

    if (!buildContext(srcW, srcH, srcFmt, srcW, srcH)) {
        LOG_E("Failed to create sws context (%dx%d, fmt=%d)",
              srcW, srcH, static_cast<int>(srcFmt));
        return false;
    }
    return true;
}

void VideoRenderer::setTargetSize(int w, int h) {
    if (w <= 0 || h <= 0) {
        w = srcW_;
        h = srcH_;
    }
    if (w == dstW_ && h == dstH_ && !pendingValid_) return;
    // 只记录 pending，不立即重建（拖动窗口时合并连续尺寸）
    pendingW_ = w;
    pendingH_ = h;
    pendingValid_ = true;
    pendingTp_ = std::chrono::steady_clock::now();
}

void VideoRenderer::applyPendingTarget() {
    if (!pendingValid_) return;
    auto elapsed = std::chrono::steady_clock::now() - pendingTp_;
    if (swsCtx_ && elapsed < std::chrono::milliseconds(kResizeDebounceMs)) {
        return; // 防抖窗口内继续用旧尺寸，UI 层 drawImage 兜底缩放
    }
    if (buildContext(srcW_, srcH_, srcFmt_, pendingW_, pendingH_)) {
        pendingValid_ = false;
    }
    // 构建失败：保留 pending，下一帧重试（旧 context 继续服务）
}

bool VideoRenderer::ensureDstBuffer(int w, int h) {
    if (dstBuf_ && bufW_ == w && bufH_ == h) return true;

    int size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, w, h, kAlign);
    if (size < 0) {
        LOG_E("av_image_get_buffer_size failed: %d", size);
        return false;
    }
    // 额外 64 字节尾部余量，双保险覆盖 SIMD 末行过量写
    uint8_t* buf = static_cast<uint8_t*>(av_malloc(size + 64));
    if (!buf) {
        LOG_E("av_malloc failed for %d bytes", size + 64);
        return false;
    }

    uint8_t* ptrs[4] = {};
    int      ls[4]   = {};
    int ret = av_image_fill_arrays(ptrs, ls, buf,
                                   AV_PIX_FMT_RGBA, w, h, kAlign);
    if (ret < 0) {
        LOG_E("av_image_fill_arrays failed: %d", ret);
        av_free(buf);
        return false;
    }

    av_free(dstBuf_);
    dstBuf_ = buf;
    std::copy(std::begin(ptrs), std::end(ptrs), std::begin(dstData_));
    std::copy(std::begin(ls),   std::end(ls),   std::begin(dstLinesize_));
    bufW_ = w;
    bufH_ = h;
    return true;
}

QImage VideoRenderer::render(AVFrame* frame) {
    if (!frame) return {};

    // 检测分辨率/格式变化，自动重建
    if (frame->width != srcW_ || frame->height != srcH_ ||
        frame->format != srcFmt_) {
        LOG_I("Video parameters changed: %dx%d fmt=%d -> %dx%d fmt=%d",
              srcW_, srcH_, static_cast<int>(srcFmt_),
              frame->width, frame->height, static_cast<int>(frame->format));
        // 源变化后宽高比可能改变：先按源尺寸输出，pending 作废，
        // 显示控件经 setFrame → contentSize 重新下发目标尺寸
        pendingValid_ = false;
        if (!buildContext(frame->width, frame->height,
                          static_cast<AVPixelFormat>(frame->format),
                          frame->width, frame->height)) {
            return {};
        }
    }

    // 窗口 resize 的防抖重建
    applyPendingTarget();
    if (!swsCtx_) return {};

    if (!ensureDstBuffer(dstW_, dstH_)) return {};

    int ret = sws_scale(swsCtx_,
                        frame->data, frame->linesize, 0, frame->height,
                        dstData_, dstLinesize_);
    if (ret < 0) {
        LOG_E("sws_scale failed: %d", ret);
        return {};
    }

    // 外部缓冲 QImage 不做引用计数，且缓冲下一帧即被覆写，
    // 这里 copy 出一份紧凑、自有的 QImage（每帧仅一次内存拷贝）
    QImage view(dstData_[0], dstW_, dstH_, dstLinesize_[0],
                QImage::Format_RGBA8888);
    return view.copy();
}

void VideoRenderer::close() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (dstBuf_) {
        av_free(dstBuf_);
        dstBuf_ = nullptr;
    }
    dstData_[0] = dstData_[1] = dstData_[2] = dstData_[3] = nullptr;
    dstLinesize_[0] = dstLinesize_[1] = dstLinesize_[2] = dstLinesize_[3] = 0;
    bufW_ = bufH_ = 0;
    srcW_ = srcH_ = 0;
    dstW_ = dstH_ = 0;
    srcFmt_ = AV_PIX_FMT_NONE;
    pendingValid_ = false;
}
