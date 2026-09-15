#ifndef BOUNDED_QUEUE_H
#define BOUNDED_QUEUE_H

#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstddef>

/**
 * @brief 线程安全有界阻塞队列
 *
 * 用于在生产者和消费者线程之间传递数据。
 * push 在队列满时阻塞等待，pop 在队列空时阻塞等待，
 * 均支持超时返回以支持优雅退出（seek/stop 场景）。
 */
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity)
        : capacity_(capacity) {}

    /**
     * @brief 尝试压入元素，队列满时等待直到超时
     * @param item 右值引用，零拷贝入队
     * @param timeout 最大等待时间，超时返回 false
     * @return true 表示成功入队，false 表示超时
     */
    bool push(T&& item, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (!notFull_.wait_for(lock, timeout, [this] { return queue_.size() < capacity_ || closed_; })) {
            return false; // 超时
        }
        if (closed_) return false;
        queue_.push_back(std::move(item));
        notEmpty_.notify_one();
        return true;
    }

    /**
     * @brief 尝试弹出元素，队列空时等待直到超时
     * @param item 输出引用，接收弹出的元素
     * @param timeout 最大等待时间，超时返回 false
     * @return true 表示成功弹出，false 表示超时或队列已关闭
     */
    bool pop(T& item, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (!notEmpty_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; })) {
            return false; // 超时
        }
        if (queue_.empty()) return false; // closed_ 且队列已空
        item = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();
        return true;
    }

    /**
     * @brief 清空队列并通知所有等待线程
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.clear();
        notFull_.notify_all();
        notEmpty_.notify_all();
    }

    /** @brief 关闭队列，所有等待立即返回 */
    void close() {
        std::lock_guard<std::mutex> lock(mtx_);
        closed_ = true;
        notFull_.notify_all();
        notEmpty_.notify_all();
    }

    /** @brief 重新打开已关闭的队列 */
    void reopen() {
        std::lock_guard<std::mutex> lock(mtx_);
        closed_ = false;
    }

    /** @brief 当前队列大小 */
    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    /** @brief 队列是否为空 */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    /** @brief 队列容量 */
    size_t capacity() const { return capacity_; }

private:
    mutable std::mutex mtx_;
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
    std::deque<T> queue_;
    size_t capacity_;
    bool closed_ = false;
};

#endif // BOUNDED_QUEUE_H