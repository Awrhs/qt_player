#ifndef PACKET_QUEUE_H
#define PACKET_QUEUE_H

#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdint>

extern "C" {
#include <libavcodec/packet.h>
}

/**
 * @brief 队列元素：AVPacket + 代际（serial）
 *
 * pkt == nullptr 表示 EOF 哨兵：解码线程据此发送 NULL 包排空解码器。
 */
struct PacketItem {
    AVPacket* pkt    = nullptr;
    uint64_t  serial = 0;
};

/**
 * @brief 解复用 → 解码线程的包队列（ffplay 风格的 serial 机制）
 *
 * - 无容量上限：由解复用线程根据 nbPackets()/durationTb()/sizeBytes()
 *   自行判断“缓冲足够”而暂停读包（所有存在的流都足够时才等待，避免
 *   一条流的队列满而另一条流饿死的互锁）。
 * - 每次 seek 调用 flush(newSerial, target)：清空并切换代际。解码线程
 *   看到元素 serial 变化即 avcodec_flush_buffers 并从 seekTarget() 取得
 *   新的 pre-roll 丢弃阈值；旧代际的包/帧一律丢弃，杜绝 seek 后播出旧数据。
 */
class PacketQueue {
public:
    PacketQueue() = default;
    ~PacketQueue() { clear(); }

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    /** @brief 入队（接管所有权）；pkt 可为 nullptr（EOF 哨兵） */
    void put(AVPacket* pkt) {
        std::lock_guard<std::mutex> lock(mtx_);
        PacketItem item;
        item.pkt    = pkt;
        item.serial = serial_;
        if (pkt) {
            size_ += pkt->size;
            if (pkt->duration > 0) duration_ += pkt->duration;
        }
        queue_.push_back(item);
        cv_.notify_one();
    }

    /** @brief 出队；超时返回 false */
    bool get(PacketItem& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            return false;
        }
        out = queue_.front();
        queue_.pop_front();
        if (out.pkt) {
            size_ -= out.pkt->size;
            if (out.pkt->duration > 0) duration_ -= out.pkt->duration;
        }
        return true;
    }

    /**
     * @brief seek：释放全部元素并切换代际
     * @param newSerial  新代际
     * @param seekTarget 目标位置（流时间，秒）；<0 表示无 pre-roll 过滤
     */
    void flush(uint64_t newSerial, double seekTarget) {
        std::lock_guard<std::mutex> lock(mtx_);
        freeAllLocked();
        serial_ = newSerial;
        target_ = seekTarget;
    }

    /** @brief 释放全部元素（代际不变） */
    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        freeAllLocked();
    }

    uint64_t serial() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return serial_;
    }

    double seekTarget() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return target_;
    }

    int nbPackets() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return static_cast<int>(queue_.size());
    }

    int64_t sizeBytes() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return size_;
    }

    /** @brief 队列中包时长之和（流 time_base 单位；无时长信息的格式为 0） */
    int64_t durationTb() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return duration_;
    }

private:
    void freeAllLocked() {
        for (auto& item : queue_) {
            if (item.pkt) av_packet_free(&item.pkt);
        }
        queue_.clear();
        size_     = 0;
        duration_ = 0;
    }

    mutable std::mutex       mtx_;
    std::condition_variable  cv_;
    std::deque<PacketItem>   queue_;
    uint64_t serial_   = 0;
    double   target_   = -1.0;
    int64_t  size_     = 0;
    int64_t  duration_ = 0;
};

#endif // PACKET_QUEUE_H
