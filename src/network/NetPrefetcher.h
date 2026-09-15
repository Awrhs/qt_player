#ifndef NET_PREFETCHER_H
#define NET_PREFETCHER_H

#include <QObject>
#include <QThread>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <atomic>
#include <memory>

#include "RingBuffer.h"

/**
 * @brief 网络预读线程
 *
 * 从 HTTP(S) URL 下载数据到 RingBuffer，供自定义 AVIOContext 消费。
 * 窗口满时暂停，有空间时恢复。
 */
class NetPrefetcher : public QObject {
    Q_OBJECT
public:
    explicit NetPrefetcher(QObject* parent = nullptr);
    ~NetPrefetcher() override;

    /**
     * @brief 开始预读
     * @param url  媒体文件 URL
     * @param ring 环形缓冲
     */
    void start(const QString& url, RingBuffer* ring);

    /** @brief 暂停预读 */
    void pause();

    /** @brief 恢复预读 */
    void resume();

    /** @brief 中止预读 */
    void abort();

    /** @brief 是否正在运行 */
    bool isRunning() const { return running_.load(); }

    /** @brief 下载进度 0.0 ~ 1.0 */
    double progress() const { return progress_.load(); }

    /** @brief 缓冲进度 0.0 ~ 1.0 */
    double bufferRatio() const;

signals:
    void downloadProgress(double ratio);
    void bufferingChanged(bool buffering);
    /** @brief 30 秒无数据超时 */
    void timeoutError();

private slots:
    void onReadyRead();
    void onFinished();
    void onDownloadProgress(qint64 received, qint64 total);
    void onError(QNetworkReply::NetworkError code);

private:
    QNetworkAccessManager* manager_ = nullptr;
    QNetworkReply*         reply_   = nullptr;
    RingBuffer*            ring_    = nullptr;
    QUrl                   url_;

    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<double> progress_{0.0};

    // 超时监控
    QTimer* timeoutTimer_ = nullptr;
    bool    hasData_ = false;
};

#endif // NET_PREFETCHER_H