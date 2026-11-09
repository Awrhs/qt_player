#include "AudioPipeline.h"
#include "../utils/Log.h"

extern "C" {
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>

LOG_TAG("AudioPipe")

namespace {
const char* errStr(int err, char* buf, size_t len) {
    av_strerror(err, buf, len);
    return buf;
}
}

AudioPipeline::AudioPipeline() {
    outFrame_ = av_frame_alloc();
}

AudioPipeline::~AudioPipeline() {
    destroyGraph();
    av_frame_free(&outFrame_);
    av_channel_layout_uninit(&inLayout_);
}

void AudioPipeline::configure(int outSampleRate, AVRational timeBase) {
    outRate_ = outSampleRate > 0 ? outSampleRate : 48000;
    tb_ = (timeBase.num > 0 && timeBase.den > 0) ? timeBase : AVRational{1, 1};
    destroyGraph();
}

bool AudioPipeline::setTempo(double tempo) {
    tempo = std::clamp(tempo, kMinTempo, kMaxTempo);
    if (graph_ && tempoCtx_ && !graphEof_) {
        char arg[32];
        std::snprintf(arg, sizeof(arg), "%.6f", tempo);
        char res[128] = {0};
        int ret = avfilter_graph_send_command(graph_, "atempo", "tempo", arg,
                                              res, sizeof(res), 0);
        if (ret < 0) {
            // 极旧版本不支持运行时命令：下一帧用新 tempo 重建滤镜图（丢失 ~1 窗口音频）
            char eb[128];
            LOG_W("atempo tempo command failed: %s; graph will be rebuilt",
                  errStr(ret, eb, sizeof(eb)));
            destroyGraph();
        }
    }
    tempo_ = tempo;
    return true;
}

void AudioPipeline::reset(double discardUntilSec) {
    destroyGraph();
    discardUntil_ = discardUntilSec;
    havePos_      = false;
    mediaPos_     = 0.0;
    lastInEnd_    = discardUntilSec >= 0.0 ? discardUntilSec : 0.0;
}

bool AudioPipeline::inputMatches(const AVFrame* f) const {
    return graph_ && !graphEof_ &&
           f->sample_rate == inRate_ &&
           f->format == static_cast<int>(inFmt_) &&
           av_channel_layout_compare(&f->ch_layout, &inLayout_) == 0;
}

bool AudioPipeline::process(const AVFrame* frame, std::vector<PcmChunk>& out) {
    if (!frame || frame->nb_samples <= 0 || frame->sample_rate <= 0 ||
        frame->ch_layout.nb_channels <= 0) {
        return true;
    }

    int64_t pts = frame->pts;
    if (pts == AV_NOPTS_VALUE) pts = frame->best_effort_timestamp;
    const double ptsSec = (pts != AV_NOPTS_VALUE) ? pts * av_q2d(tb_) : lastInEnd_;
    const double frameDur = static_cast<double>(frame->nb_samples) / frame->sample_rate;

    // seek pre-roll：整帧早于目标点 → 丢；跨越目标点 → 裁掉开头样本
    int skip = 0;
    if (discardUntil_ >= 0.0) {
        if (ptsSec + frameDur <= discardUntil_) {
            lastInEnd_ = ptsSec + frameDur;
            return true;
        }
        if (ptsSec < discardUntil_) {
            skip = static_cast<int>((discardUntil_ - ptsSec) * frame->sample_rate + 0.5);
            skip = std::clamp(skip, 0, frame->nb_samples - 1);
        }
        discardUntil_ = -1.0;
    }
    // 时间戳间断（TS 断流、丢包、解码错误导致的帧丢失）：排空旧图尾部并重新锚定，
    // 否则按输出样本累加的媒体时间会与真实时间戳永久错位
    if (havePos_ && graph_ && !graphEof_ && pts != AV_NOPTS_VALUE &&
        std::fabs(ptsSec - lastInEnd_) > kGapThresholdSec) {
        LOG_I("audio timestamp discontinuity: expected %.3f got %.3f, re-anchoring",
              lastInEnd_, ptsSec);
        flush(out);
        havePos_ = false;
    }
    lastInEnd_ = ptsSec + frameDur;

    if (!inputMatches(frame)) {
        if (graph_ && !graphEof_) {
            // 输入参数中途变化：先排空旧图尾部
            flush(out);
        }
        if (!buildGraph(frame)) return false;
    }

    const double startSec = ptsSec + static_cast<double>(skip) / frame->sample_rate;
    if (!havePos_) {
        mediaPos_ = startSec;
        havePos_  = true;
    }

    AVFrame* in = av_frame_clone(frame);
    if (!in) return false;

    if (skip > 0) {
        const auto sfmt   = static_cast<AVSampleFormat>(in->format);
        const int  bps    = av_get_bytes_per_sample(sfmt);
        const int  nch    = in->ch_layout.nb_channels;
        const bool planar = av_sample_fmt_is_planar(sfmt);
        if (planar) {
            for (int ch = 0; ch < nch; ++ch) {
                if (in->extended_data[ch]) {
                    in->extended_data[ch] += static_cast<std::ptrdiff_t>(skip) * bps;
                }
            }
            if (in->extended_data != in->data) {
                for (int ch = 0; ch < std::min(nch, AV_NUM_DATA_POINTERS); ++ch) {
                    in->data[ch] = in->extended_data[ch];
                }
            }
        } else {
            in->data[0] += static_cast<std::ptrdiff_t>(skip) * bps * nch;
            if (in->extended_data != in->data) in->extended_data[0] = in->data[0];
        }
        in->nb_samples -= skip;
    }

    // 滤镜内 pts 使用样本计数（time_base = 1/采样率），与流 pts 解耦
    in->pts = fedSamples_;
    fedSamples_ += in->nb_samples;

    int ret = av_buffersrc_add_frame_flags(src_, in, 0);
    av_frame_free(&in);
    if (ret < 0) {
        char eb[128];
        LOG_E("av_buffersrc_add_frame failed: %s", errStr(ret, eb, sizeof(eb)));
        return false;
    }
    return pullOutputs(out);
}

bool AudioPipeline::flush(std::vector<PcmChunk>& out) {
    if (!graph_ || graphEof_) return true;
    int ret = av_buffersrc_add_frame_flags(src_, nullptr, 0);
    if (ret < 0) {
        char eb[128];
        LOG_W("buffersrc EOF signal failed: %s", errStr(ret, eb, sizeof(eb)));
        graphEof_ = true;
        return false;
    }
    bool ok = pullOutputs(out);
    graphEof_ = true;
    return ok;
}

bool AudioPipeline::pullOutputs(std::vector<PcmChunk>& out) {
    for (;;) {
        int ret = av_buffersink_get_frame(sink_, outFrame_);
        if (ret == AVERROR(EAGAIN)) return true;
        if (ret == AVERROR_EOF) {
            graphEof_ = true;
            return true;
        }
        if (ret < 0) {
            char eb[128];
            LOG_E("av_buffersink_get_frame failed: %s", errStr(ret, eb, sizeof(eb)));
            return false;
        }
        if (outFrame_->nb_samples > 0 && outFrame_->data[0] &&
            outFrame_->format == AV_SAMPLE_FMT_FLT &&
            outFrame_->ch_layout.nb_channels == kOutChannels) {
            PcmChunk c;
            c.pts   = mediaPos_;
            c.tempo = tempo_;
            const float* p = reinterpret_cast<const float*>(outFrame_->data[0]);
            c.data.assign(p, p + static_cast<size_t>(outFrame_->nb_samples) * kOutChannels);
            mediaPos_ += static_cast<double>(outFrame_->nb_samples) * tempo_ / outRate_;
            out.push_back(std::move(c));
        } else if (outFrame_->nb_samples > 0) {
            LOG_E("unexpected sink frame format=%d ch=%d", outFrame_->format,
                  outFrame_->ch_layout.nb_channels);
        }
        av_frame_unref(outFrame_);
    }
}

bool AudioPipeline::buildGraph(const AVFrame* t) {
    destroyGraph();

    graph_ = avfilter_graph_alloc();
    if (!graph_) return false;
    graph_->nb_threads = 1;

    const AVFilter* abuffer = avfilter_get_by_name("abuffer");
    const AVFilter* aformat = avfilter_get_by_name("aformat");
    const AVFilter* atempo  = avfilter_get_by_name("atempo");
    const AVFilter* absink  = avfilter_get_by_name("abuffersink");
    if (!abuffer || !aformat || !atempo || !absink) {
        LOG_E("required audio filters missing (abuffer/aformat/atempo/abuffersink)");
        destroyGraph();
        return false;
    }

    char eb[128];
    int ret = 0;

    src_ = avfilter_graph_alloc_filter(graph_, abuffer, "src");
    if (!src_) { destroyGraph(); return false; }
    {
        AVBufferSrcParameters* par = av_buffersrc_parameters_alloc();
        if (!par) { destroyGraph(); return false; }
        par->format      = t->format;
        par->sample_rate = t->sample_rate;
        par->time_base   = AVRational{1, t->sample_rate};
        av_channel_layout_copy(&par->ch_layout, &t->ch_layout);
        ret = av_buffersrc_parameters_set(src_, par);
        av_channel_layout_uninit(&par->ch_layout);
        av_free(par);
        if (ret < 0) {
            LOG_E("av_buffersrc_parameters_set failed: %s", errStr(ret, eb, sizeof(eb)));
            destroyGraph();
            return false;
        }
    }
    ret = avfilter_init_str(src_, nullptr);
    if (ret < 0) {
        LOG_E("abuffer init failed: %s", errStr(ret, eb, sizeof(eb)));
        destroyGraph();
        return false;
    }

    char fargs[160];
    std::snprintf(fargs, sizeof(fargs),
                  "sample_fmts=flt:channel_layouts=stereo:sample_rates=%d", outRate_);
    ret = avfilter_graph_create_filter(&fmt_, aformat, "aformat", fargs, nullptr, graph_);
    if (ret < 0) {
        LOG_E("aformat create failed: %s", errStr(ret, eb, sizeof(eb)));
        destroyGraph();
        return false;
    }

    char targs[64];
    std::snprintf(targs, sizeof(targs), "tempo=%.6f", tempo_);
    ret = avfilter_graph_create_filter(&tempoCtx_, atempo, "atempo", targs, nullptr, graph_);
    if (ret < 0) {
        LOG_E("atempo create failed: %s", errStr(ret, eb, sizeof(eb)));
        destroyGraph();
        return false;
    }

    ret = avfilter_graph_create_filter(&sink_, absink, "sink", nullptr, nullptr, graph_);
    if (ret < 0) {
        LOG_E("abuffersink create failed: %s", errStr(ret, eb, sizeof(eb)));
        destroyGraph();
        return false;
    }

    if ((ret = avfilter_link(src_, 0, fmt_, 0)) < 0 ||
        (ret = avfilter_link(fmt_, 0, tempoCtx_, 0)) < 0 ||
        (ret = avfilter_link(tempoCtx_, 0, sink_, 0)) < 0) {
        LOG_E("avfilter_link failed: %s", errStr(ret, eb, sizeof(eb)));
        destroyGraph();
        return false;
    }

    ret = avfilter_graph_config(graph_, nullptr);
    if (ret < 0) {
        LOG_E("avfilter_graph_config failed: %s", errStr(ret, eb, sizeof(eb)));
        destroyGraph();
        return false;
    }

    inRate_ = t->sample_rate;
    inFmt_  = static_cast<AVSampleFormat>(t->format);
    av_channel_layout_uninit(&inLayout_);
    av_channel_layout_copy(&inLayout_, &t->ch_layout);
    graphEof_   = false;
    fedSamples_ = 0;

    char layoutStr[64] = {0};
    av_channel_layout_describe(&inLayout_, layoutStr, sizeof(layoutStr));
    LOG_I("audio graph: %dHz %s %s -> %dHz stereo flt, tempo=%.2f",
          inRate_, layoutStr, av_get_sample_fmt_name(inFmt_), outRate_, tempo_);
    return true;
}

void AudioPipeline::destroyGraph() {
    if (graph_) avfilter_graph_free(&graph_);
    graph_    = nullptr;
    src_      = nullptr;
    fmt_      = nullptr;
    tempoCtx_ = nullptr;
    sink_     = nullptr;
    graphEof_ = false;
    inRate_   = 0;
    inFmt_    = AV_SAMPLE_FMT_NONE;
}
