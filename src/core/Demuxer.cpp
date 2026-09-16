#include "Demuxer.h"
#include "../utils/Log.h"

LOG_TAG("Demuxer")

Demuxer::Demuxer()  = default;
Demuxer::~Demuxer() { close(); }

bool Demuxer::open(const std::string& url) {
    close();

    // 打开输入文件/流
    AVDictionary* opts = nullptr;
    // 网络流优化：缩短探测时间
    av_dict_set(&opts, "analyzeduration", "2000000", 0);  // 2秒探测窗口
    av_dict_set(&opts, "probesize",        "5000000", 0);  // 5MB 探测上限

    int ret = avformat_open_input(&fmtCtx_, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errBuf[256] = {0};
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_E("avformat_open_input failed for '%s': %s", url.c_str(), errBuf);
        return false;
    }

    // 解析流信息
    ret = avformat_find_stream_info(fmtCtx_, nullptr);
    if (ret < 0) {
        LOG_W("avformat_find_stream_info returned %d", ret);
        // 非致命：仍可尝试读取
    }

    findStreamIndices();

    LOG_I("Opened '%s': %lld streams, duration=%.2fs, video_idx=%d, audio_idx=%d",
          url.c_str(), fmtCtx_->nb_streams, durationSec(), videoIndex_, audioIndex_);

    if (videoIndex_ < 0 && audioIndex_ < 0) {
        LOG_E("No audio or video stream found");
        close();
        return false;
    }

    return true;
}

ReadStatus Demuxer::readPacket(AVPacket*& pkt) {
    pkt = nullptr;
    if (!fmtCtx_) return ReadStatus::Error;

    pkt = av_packet_alloc();
    if (!pkt) return ReadStatus::Error;

    int ret = av_read_frame(fmtCtx_, pkt);
    if (ret < 0) {
        av_packet_free(&pkt);
        if (ret == AVERROR_EOF) {
            return ReadStatus::Eof;
        }
        char errBuf[256] = {0};
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_W("av_read_frame error: %s", errBuf);
        return ReadStatus::Error;
    }
    return ReadStatus::Ok;
}

void Demuxer::close() {
    if (fmtCtx_) {
        avformat_close_input(&fmtCtx_);
        fmtCtx_ = nullptr;
    }
    audioIndex_ = -1;
    videoIndex_ = -1;
}

double Demuxer::durationSec() const {
    if (!fmtCtx_ || fmtCtx_->duration == AV_NOPTS_VALUE)
        return 0.0;
    return fmtCtx_->duration / (double)AV_TIME_BASE;
}

AVRational Demuxer::audioTimeBase() const {
    if (audioIndex_ < 0) return {0, 0};
    return fmtCtx_->streams[audioIndex_]->time_base;
}

AVRational Demuxer::videoTimeBase() const {
    if (videoIndex_ < 0) return {0, 0};
    return fmtCtx_->streams[videoIndex_]->time_base;
}

void Demuxer::findStreamIndices() {
    audioIndex_ = -1;
    videoIndex_ = -1;
    for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
        AVMediaType type = fmtCtx_->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_AUDIO && audioIndex_ < 0)
            audioIndex_ = i;
        else if (type == AVMEDIA_TYPE_VIDEO && videoIndex_ < 0)
            videoIndex_ = i;
    }
}