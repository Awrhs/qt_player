#include "Demuxer.h"
#include "../utils/Log.h"

#include <algorithm>

LOG_TAG("Demuxer")

Demuxer::Demuxer()  = default;
Demuxer::~Demuxer() { close(); }

int Demuxer::interruptCallback(void* opaque) {
    auto* self = static_cast<Demuxer*>(opaque);
    return (self && self->abort_ && self->abort_->load()) ? 1 : 0;
}

bool Demuxer::open(const std::string& url) {
    close();

    fmtCtx_ = avformat_alloc_context();
    if (!fmtCtx_) return false;
    fmtCtx_->interrupt_callback.callback = &Demuxer::interruptCallback;
    fmtCtx_->interrupt_callback.opaque   = this;

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
        fmtCtx_ = nullptr;   // avformat_open_input 失败时已释放 context
        return false;
    }

    ret = avformat_find_stream_info(fmtCtx_, nullptr);
    if (ret < 0) {
        LOG_W("avformat_find_stream_info returned %d", ret);
        // 非致命：仍可尝试读取
    }

    findStreamIndices();

    LOG_I("Opened '%s': %u streams, start=%.3fs, duration=%.2fs, video_idx=%d, audio_idx=%d",
          url.c_str(), fmtCtx_->nb_streams, startTimeSec(), durationSec(), videoIndex_, audioIndex_);

    if (videoIndex_ < 0 && audioIndex_ < 0) {
        LOG_E("No audio or video stream found");
        close();
        return false;
    }

    // 已选定的流以外的流全部丢弃，解复用器不必为它们解析/排队
    for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
        if (static_cast<int>(i) != videoIndex_ && static_cast<int>(i) != audioIndex_) {
            fmtCtx_->streams[i]->discard = AVDISCARD_ALL;
        }
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
        if (ret == AVERROR_EOF || (fmtCtx_->pb && avio_feof(fmtCtx_->pb))) {
            return ReadStatus::Eof;
        }
        if (ret == AVERROR(EAGAIN)) {
            return ReadStatus::Again;
        }
        char errBuf[256] = {0};
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_W("av_read_frame error: %s", errBuf);
        return ReadStatus::Error;
    }
    return ReadStatus::Ok;
}

bool Demuxer::seek(double targetSec) {
    if (!fmtCtx_) return false;
    int64_t ts = static_cast<int64_t>(targetSec * AV_TIME_BASE);
    int ret = av_seek_frame(fmtCtx_, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        char errBuf[256] = {0};
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOG_W("av_seek_frame(%.3f) failed: %s", targetSec, errBuf);
        return false;
    }
    return true;
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
    if (!fmtCtx_) return 0.0;
    if (fmtCtx_->duration != AV_NOPTS_VALUE && fmtCtx_->duration > 0) {
        return fmtCtx_->duration / static_cast<double>(AV_TIME_BASE);
    }
    double best = 0.0;
    for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
        const AVStream* st = fmtCtx_->streams[i];
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
            best = std::max(best, st->duration * av_q2d(st->time_base));
        }
    }
    return best;
}

double Demuxer::startTimeSec() const {
    if (!fmtCtx_ || fmtCtx_->start_time == AV_NOPTS_VALUE) return 0.0;
    return fmtCtx_->start_time / static_cast<double>(AV_TIME_BASE);
}

AVRational Demuxer::audioTimeBase() const {
    if (audioIndex_ < 0) return {0, 1};
    return fmtCtx_->streams[audioIndex_]->time_base;
}

AVRational Demuxer::videoTimeBase() const {
    if (videoIndex_ < 0) return {0, 1};
    return fmtCtx_->streams[videoIndex_]->time_base;
}

double Demuxer::videoFrameRate() const {
    if (videoIndex_ < 0) return 0.0;
    const AVStream* st = fmtCtx_->streams[videoIndex_];
    AVRational fr = st->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) return 0.0;
    return av_q2d(fr);
}

bool Demuxer::videoIsAttachedPic() const {
    if (videoIndex_ < 0) return false;
    return (fmtCtx_->streams[videoIndex_]->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
}

bool Demuxer::isSeekable() const {
    if (!fmtCtx_) return false;
    if (fmtCtx_->pb && !(fmtCtx_->pb->seekable & AVIO_SEEKABLE_NORMAL)) return false;
    return true;
}

void Demuxer::findStreamIndices() {
    audioIndex_ = -1;
    videoIndex_ = -1;
    int v = av_find_best_stream(fmtCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int a = av_find_best_stream(fmtCtx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (v >= 0) videoIndex_ = v;
    if (a >= 0) audioIndex_ = a;
}
