#include "Decoder.h"
#include "../utils/Log.h"

#include <cstring>

LOG_TAG("Decoder")

Decoder::Decoder()  = default;
Decoder::~Decoder() { close(); }

bool Decoder::open(const AVCodecParameters* codecpar, AVRational timeBase, bool useHW) {
    close();
    useHW_ = useHW;

    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        LOG_E("Unsupported codec: %d", codecpar->codec_id);
        return false;
    }

    auto allocCtx = [&]() -> bool {
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) {
            LOG_E("Failed to allocate AVCodecContext");
            return false;
        }
        int r = avcodec_parameters_to_context(ctx_, codecpar);
        if (r < 0) {
            LOG_E("avcodec_parameters_to_context failed: %d", r);
            return false;
        }
        if (timeBase.num > 0 && timeBase.den > 0) ctx_->pkt_timebase = timeBase;
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            // 多线程软解（自动按 CPU 数），1080p H.264 单线程常常跟不上实时
            ctx_->thread_count = 0;
            ctx_->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        return true;
    };

    if (!allocCtx()) {
        close();
        return false;
    }

    // 尝试硬件解码
    if (useHW_ && codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (tryInitHardware(codec)) {
            ctx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
            ctx_->get_format    = getHWFormat;
            ctx_->opaque        = this;
            LOG_I("Hardware decode enabled (DXVA2)");
        } else {
            LOG_W("Hardware decode unavailable, falling back to software");
            useHW_ = false;
            avcodec_free_context(&ctx_);
            if (!allocCtx()) {
                close();
                return false;
            }
        }
    }

    int ret = avcodec_open2(ctx_, codec, nullptr);
    if (ret < 0) {
        char errBuf[256] = {0};
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_E("avcodec_open2 failed: %s", errBuf);
        close();
        return false;
    }

    LOG_I("Decoder opened: codec=%s, hw=%d, threads=%d", codec->name, useHW_, ctx_->thread_count);
    return true;
}

int Decoder::sendPacket(const AVPacket* pkt) {
    if (!ctx_) return AVERROR(EINVAL);
    return avcodec_send_packet(ctx_, pkt);
}

int Decoder::receiveFrame(AVFrame* frame) {
    if (!ctx_) return AVERROR(EINVAL);
    int ret = avcodec_receive_frame(ctx_, frame);

    // 硬解：回读到系统内存
    if (ret == 0 && useHW_ && frame->format == hwPixFmt_) {
        AVFrame* swFrame = av_frame_alloc();
        if (!swFrame) return AVERROR(ENOMEM);
        int hwRet = av_hwframe_transfer_data(swFrame, frame, 0);
        if (hwRet < 0) {
            av_frame_free(&swFrame);
            LOG_E("av_hwframe_transfer_data failed: %d", hwRet);
            return hwRet;
        }
        av_frame_copy_props(swFrame, frame);   // pts / duration / SAR / 色彩信息等
        av_frame_unref(frame);
        av_frame_move_ref(frame, swFrame);
        av_frame_free(&swFrame);
    }

    return ret;
}

void Decoder::flush() {
    if (ctx_) avcodec_flush_buffers(ctx_);
}

void Decoder::close() {
    if (ctx_) {
        avcodec_free_context(&ctx_);
    }
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
    }
    hwPixFmt_ = AV_PIX_FMT_NONE;
    useHW_ = false;
}

bool Decoder::tryInitHardware(const AVCodec* codec) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) break;
        if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            cfg->device_type == AV_HWDEVICE_TYPE_DXVA2) {
            hwPixFmt_ = cfg->pix_fmt;
            break;
        }
    }
    if (hwPixFmt_ == AV_PIX_FMT_NONE) {
        LOG_W("Codec does not support DXVA2");
        return false;
    }

    int ret = av_hwdevice_ctx_create(&hwDeviceCtx_, AV_HWDEVICE_TYPE_DXVA2, nullptr, nullptr, 0);
    if (ret < 0) {
        LOG_W("av_hwdevice_ctx_create(DXVA2) failed: %d", ret);
        hwPixFmt_ = AV_PIX_FMT_NONE;
        return false;
    }
    return true;
}

AVPixelFormat Decoder::getHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    Decoder* dec = static_cast<Decoder*>(ctx->opaque);
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == dec->hwPixFmt_)
            return *p;
    }
    LOG_W("HW pixel format not supported by decoder, falling back");
    return pix_fmts[0];
}
