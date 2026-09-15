#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <vector>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <algorithm>

/**
 * @brief 线程安全环形缓冲
 *
 * 用于网络下载线程和 AVIOContext 之间的数据传递。
 * 支持多读/多写，自动处理环绕逻辑。
 */
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 4 * 1024 * 1024)  // 默认 4MB
        : buffer_(capacity),
          capacity_(capacity),
          writePos_(0),
          readPos_(0),
          filled_(false) {}

    /**
     * @brief 写入数据
     * @param data 源数据
     * @param len  长度
     * @return 实际写入字节数（<= len）
     */
    size_t write(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(mtx_);
        size_t available = capacity_ - sizeLocked();
        size_t toWrite = std::min(len, available);

        // 分两段写入（环绕）
        size_t first = std::min(toWrite, capacity_ - (writePos_ % capacity_));
        std::memcpy(&buffer_[writePos_ % capacity_], data, first);
        if (first < toWrite) {
            std::memcpy(buffer_.data(), data + first, toWrite - first);
        }

        writePos_ += toWrite;
        filled_ = filled_ || (sizeLocked() >= capacity_);

        cond_.notify_all();
        return toWrite;
    }

    /**
     * @brief 读取数据
     * @param data 目标缓冲
     * @param len  请求长度
     * @return 实际读取字节数（<= len）
     */
    size_t read(uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(mtx_);
        size_t avail = sizeLocked();
        size_t toRead = std::min(len, avail);

        size_t first = std::min(toRead, capacity_ - (readPos_ % capacity_));
        std::memcpy(data, &buffer_[readPos_ % capacity_], first);
        if (first < toRead) {
            std::memcpy(data + first, buffer_.data(), toRead - first);
        }

        readPos_ += toRead;
        return toRead;
    }

    /** @brief 等待直到有可用数据 */
    size_t waitForData(size_t minBytes, int timeoutMs = 3000) {
        std::unique_lock<std::mutex> lock(mtx_);
        cond_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this, minBytes] {
            return sizeLocked() >= minBytes || aborted_;
        });
        if (aborted_) return 0;
        return sizeLocked();
    }

    /** @brief 当前可读字节数 */
    size_t available() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return sizeLocked();
    }

    /** @brief 空闲空间 */
    size_t freeSpace() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return capacity_ - sizeLocked();
    }

    /** @brief 清空 */
    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        writePos_ = 0;
        readPos_ = 0;
        filled_ = false;
        aborted_ = false;
    }

    /** @brief 中止等待 */
    void abort() {
        std::lock_guard<std::mutex> lock(mtx_);
        aborted_ = true;
        cond_.notify_all();
    }

private:
    /** @brief 当前有效数据大小（调用方已持有锁） */
    size_t sizeLocked() const {
        if (writePos_ >= readPos_) return writePos_ - readPos_;
        return filled_ ? capacity_ : 0;
    }

    std::vector<uint8_t> buffer_;
    size_t capacity_;
    size_t writePos_;
    size_t readPos_;
    bool   filled_ = false;
    bool   aborted_ = false;
    mutable std::mutex mtx_;
    std::condition_variable cond_;
};

#endif // RING_BUFFER_H