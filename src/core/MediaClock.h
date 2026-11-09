#ifndef MEDIA_CLOCK_H
#define MEDIA_CLOCK_H

#include <chrono>
#include <mutex>

/**
 * @brief 统一播放主时钟（单位：秒，流时间，含容器 start_time）
 *
 * 有音频流时：音频输出每次把 PCM 交付给设备后调用 update()，把时钟锚定到
 * 该 PCM 的媒体时间，并按 speed 在两次设备回调之间用墙上时钟插值；
 * 无音频流时：渲染线程在首帧上屏时 update()，之后按 speed 随墙上时钟推进。
 *
 * 三种“不走时”状态：
 *  - waiting：set() 之后尚未收到任何 update()（打开/seek 后等待首个数据），
 *             now() 恒等于 set 的位置，避免解码预热期间时钟空跑导致开头丢帧；
 *  - frozen ：消费端数据饥饿（freeze()），保持当前位置，下一次 update() 恢复；
 *  - paused ：用户暂停，resume() 后从暂停位置继续。
 *
 * 所有方法线程安全。
 */
class MediaClock {
public:
    MediaClock() = default;

    /** @brief 当前时钟值（秒） */
    double now() const;

    /** @brief 打开/seek：锚定到 pos 并进入 waiting，直到下一次 update() */
    void set(double pos);

    /**
     * @brief 消费端锚定并走时
     * @param pos   刚交付给设备（或刚上屏）的数据对应的媒体时间
     * @param speed 该数据的播放速率（1.0 = 正常）
     */
    void update(double pos, double speed);

    /** @brief 数据饥饿：停表并保持位置（下一次 update() 恢复走时） */
    void freeze();

    /** @brief 用户暂停：冻结在当前位置 */
    void pause();

    /** @brief 恢复：从暂停位置继续走时（waiting/frozen 状态不受影响） */
    void resume();

    /** @brief 设置插值速率（保持当前位置连续） */
    void setSpeed(double speed);

    double speed()     const;
    bool   isWaiting() const;
    bool   isPaused()  const;
    /** @brief 是否正在随墙上时钟推进（非 waiting/frozen/paused） */
    bool   isRunning() const;

private:
    double nowLocked() const;

    mutable std::mutex mtx_;
    double pos_   = 0.0;
    std::chrono::steady_clock::time_point t0_{};
    double speed_ = 1.0;
    bool running_ = false;
    bool paused_  = false;
    bool waiting_ = true;
};

#endif // MEDIA_CLOCK_H
