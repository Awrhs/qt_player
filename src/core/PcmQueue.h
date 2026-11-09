#ifndef PCM_QUEUE_H
#define PCM_QUEUE_H

#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <algorithm>

/**
 * @brief 一段交错 float PCM（设备采样率）及其媒体时间信息
 *
 * pts   ：该块第一个样本对应的媒体时间（秒，流时间）
 * tempo ：生成该块时的倍速；块内第 n 个样本的媒体时间 = pts + n * tempo / rate
 */
struct PcmChunk {
    std::vector<float> data;
    double pts   = 0.0;
    double tempo = 1.0;

    size_t frames(int channels) const {
        return channels > 0 ? data.size() / static_cast<size_t>(channels) : 0;
    }
};

/**
 * @brief 解码线程 → 音频设备回调 的有界 PCM 队列
 *
 * - 以“帧数”限容（约 250ms），而不是块数：块大小随编码/倍速变化很大。
 *   生产者队列满时阻塞形成背压；变速后约 250ms 即生效。
 * - 代际 generation：flush()（seek/停止）自增并清空。push 在队列锁内比对
 *   代际，保证 seek 后在途的旧位置 PCM 绝不会落入新队列。
 * - pull() 供消费端（设备回调 / 空设备定时器）部分消费块，并返回已交付
 *   数据末尾的媒体时间，用于驱动 MediaClock。
 */
class PcmQueue {
public:
    explicit PcmQueue(int channels = 2, int sampleRate = 48000,
                      size_t capacityFrames = 12000)
        : channels_(channels), rate_(sampleRate), capacity_(capacityFrames) {}

    void setFormat(int channels, int sampleRate) {
        std::lock_guard<std::mutex> lock(mtx_);
        channels_ = channels > 0 ? channels : 2;
        rate_     = sampleRate > 0 ? sampleRate : 48000;
    }

    void setCapacityFrames(size_t frames) {
        std::lock_guard<std::mutex> lock(mtx_);
        capacity_ = std::max<size_t>(frames, 1);
        notFull_.notify_all();
    }

    uint64_t generation() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return generation_;
    }

    /**
     * @brief 生产者入队
     * @param chunk   PCM 块（成功时被移动）
     * @param gen     生产者看到的代际；与当前代际不一致则立即返回 false
     * @param timeout 队列满时的最长等待
     * @return true 已入队；false 超时或代际已变（块仍归调用方所有）
     */
    bool push(PcmChunk&& chunk, uint64_t gen, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (gen != generation_) return false;
        bool ok = notFull_.wait_for(lock, timeout, [this, gen] {
            return gen != generation_ || totalFrames_ < capacity_ || queue_.empty();
        });
        if (!ok || gen != generation_) return false;
        totalFrames_ += chunk.frames(channels_);
        queue_.push_back(std::move(chunk));
        return true;
    }

    struct PullResult {
        size_t frames   = 0;     ///< 实际拷贝的帧数
        double endPos   = 0.0;   ///< 已交付数据末尾的媒体时间
        double tempo    = 1.0;   ///< 最后一块的倍速
        double firstPts = -1.0;  ///< 本次首次开始消费的块的 pts（无则 <0）
        bool   any      = false; ///< 是否拿到了任何数据
    };

    /**
     * @brief 消费端拉取最多 frames 帧交错 PCM（非阻塞）
     *
     * 块可被部分消费，余量留到下次。不足的部分由调用方补静音。
     */
    PullResult pull(float* out, size_t frames) {
        std::lock_guard<std::mutex> lock(mtx_);
        PullResult r;
        size_t done = 0;
        while (done < frames) {
            if (!hasCur_) {
                if (queue_.empty()) break;
                cur_ = std::move(queue_.front());
                queue_.pop_front();
                totalFrames_ -= std::min(totalFrames_, cur_.frames(channels_));
                curOff_ = 0;
                hasCur_ = true;
                if (r.firstPts < 0.0) r.firstPts = cur_.pts;
            }
            size_t curFrames = cur_.frames(channels_);
            size_t avail = curFrames - curOff_;
            size_t n = std::min(avail, frames - done);
            if (n > 0) {
                std::memcpy(out + done * channels_,
                            cur_.data.data() + curOff_ * channels_,
                            n * channels_ * sizeof(float));
                done    += n;
                curOff_ += n;
                r.endPos = cur_.pts + static_cast<double>(curOff_) * cur_.tempo / rate_;
                r.tempo  = cur_.tempo;
                r.any    = true;
            }
            if (curOff_ >= curFrames) {
                hasCur_ = false;
                cur_.data.clear();
            }
        }
        r.frames = done;
        if (done > 0) notFull_.notify_all();
        return r;
    }

    /** @brief seek/停止：切换代际并丢弃全部数据（含部分消费的块） */
    void flush() {
        std::lock_guard<std::mutex> lock(mtx_);
        ++generation_;
        queue_.clear();
        totalFrames_ = 0;
        hasCur_ = false;
        curOff_ = 0;
        cur_.data.clear();
        notFull_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty() && !hasCur_;
    }

    /** @brief 排队中的帧数（不含部分消费块的余量） */
    size_t queuedFrames() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return totalFrames_;
    }

    int channels()   const { return channels_; }
    int sampleRate() const { return rate_; }

private:
    mutable std::mutex       mtx_;
    std::condition_variable  notFull_;
    std::deque<PcmChunk>     queue_;
    PcmChunk cur_;
    size_t   curOff_      = 0;
    bool     hasCur_      = false;
    size_t   totalFrames_ = 0;
    uint64_t generation_  = 0;
    int      channels_;
    int      rate_;
    size_t   capacity_;
};

#endif // PCM_QUEUE_H
