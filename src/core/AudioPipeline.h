#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include <vector>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavfilter/avfilter.h>
}

#include "PcmQueue.h"

/**
 * @brief 音频处理管线：解码帧 → 格式转换 → 变速（保持音调）→ 交错 float 立体声 PCM
 *
 * 基于 libavfilter：abuffer → aformat(flt/stereo/设备采样率) → atempo → abuffersink。
 *  - 变速用 atempo（WSOLA）实现，不改变音调；运行中通过 avfilter_graph_send_command
 *    平滑切换 tempo，无需重建滤镜图、不丢样本。
 *  - 每个输出块携带媒体时间 pts（流时间）与倍速 tempo，音频输出据此驱动主时钟：
 *    mediaPos 累加 = 输出样本数 × tempo / 设备采样率。
 *  - seek pre-roll：reset(discardUntil) 后，整帧早于目标点的输入直接丢弃，跨越目标点
 *    的帧裁掉开头的样本，实现样本精度的起播点。
 *
 * 非线程安全：调用方（AudioOutput）负责加锁。
 */
class AudioPipeline {
public:
    static constexpr int    kOutChannels = 2;
    static constexpr double kMinTempo    = 0.5;
    static constexpr double kMaxTempo    = 2.0;

    AudioPipeline();
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    /**
     * @brief 配置输出采样率与输入帧的时间基（流 time_base）
     *
     * 输入格式不在此指定：滤镜图按首个输入帧的实际参数惰性构建，参数变化时自动重建。
     */
    void configure(int outSampleRate, AVRational timeBase);

    /** @brief 设置倍速（0.5~2.0）；滤镜图存在时即时生效 */
    bool setTempo(double tempo);
    double tempo() const { return tempo_; }

    /**
     * @brief seek：丢弃滤镜图内部状态，设置 pre-roll 丢弃阈值
     * @param discardUntilSec 早于该媒体时间（秒）的样本全部丢弃；<0 不过滤
     */
    void reset(double discardUntilSec);

    /**
     * @brief 处理一帧解码音频，产出的 PCM 块追加到 out
     * @return false 表示滤镜错误（调用方可忽略该帧）
     */
    bool process(const AVFrame* frame, std::vector<PcmChunk>& out);

    /** @brief EOF：排空滤镜图尾部（atempo/重采样器内部延迟） */
    bool flush(std::vector<PcmChunk>& out);

    /** @brief 已产出 PCM 末尾对应的媒体时间（秒） */
    double mediaPosition() const { return mediaPos_; }
    bool   hasPosition()   const { return havePos_; }
    int    outputSampleRate() const { return outRate_; }

private:
    bool buildGraph(const AVFrame* templ);
    void destroyGraph();
    bool pullOutputs(std::vector<PcmChunk>& out);
    bool inputMatches(const AVFrame* f) const;

    int        outRate_ = 48000;
    AVRational tb_{1, 1};
    double     tempo_ = 1.0;

    double discardUntil_ = -1.0;
    bool   havePos_      = false;
    double mediaPos_     = 0.0;
    double lastInEnd_    = 0.0;   // 无时间戳帧的兜底 pts
    int64_t fedSamples_  = 0;     // 喂入滤镜图的样本计数（作为滤镜内 pts）
    bool   graphEof_     = false;

    AVFilterGraph*   graph_    = nullptr;
    AVFilterContext* src_      = nullptr;
    AVFilterContext* fmt_      = nullptr;
    AVFilterContext* tempoCtx_ = nullptr;
    AVFilterContext* sink_     = nullptr;

    int             inRate_ = 0;
    AVSampleFormat  inFmt_  = AV_SAMPLE_FMT_NONE;
    AVChannelLayout inLayout_{};

    AVFrame* outFrame_ = nullptr;
};

#endif // AUDIO_PIPELINE_H
