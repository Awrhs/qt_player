#include "NetPrefetcher.h"
#include "../utils/Log.h"

#include <QTimer>
#include <QNetworkRequest>

LOG_TAG("NetPrefetcher")

NetPrefetcher::NetPrefetcher(QObject* parent)
    : QObject(parent)
{
    manager_ = new QNetworkAccessManager(this);

    timeoutTimer_ = new QTimer(this);
    timeoutTimer_->setSingleShot(true);
    timeoutTimer_->setInterval(30000); // 30s
    connect(timeoutTimer_, &QTimer::timeout, this, [this]() {
        if (!hasData_) {
            LOG_E("Network timeout: no data for 30s");
            emit timeoutError();
            abort();
        }
        hasData_ = false;
        timeoutTimer_->start();
    });
}

NetPrefetcher::~NetPrefetcher() {
    abort();
}

void NetPrefetcher::start(const QString& url, RingBuffer* ring) {
    abort();
    url_ = QUrl(url);
    ring_ = ring;
    running_ = true;
    paused_ = false;
    progress_ = 0.0;
    hasData_ = false;

    QNetworkRequest req(url_);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    // 支持 seek 需要 Range 请求，此处简单全量下载
    // 更好的方法：支持 Range 头

    reply_ = manager_->get(req);
    connect(reply_, &QNetworkReply::readyRead, this, &NetPrefetcher::onReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &NetPrefetcher::onFinished);
    connect(reply_, &QNetworkReply::downloadProgress, this, &NetPrefetcher::onDownloadProgress);
    connect(reply_, &QNetworkReply::errorOccurred, this, &NetPrefetcher::onError);

    timeoutTimer_->start();
    LOG_I("NetPrefetcher started: %s", url.toUtf8().constData());
}

void NetPrefetcher::pause() {
    paused_ = true;
}

void NetPrefetcher::resume() {
    paused_ = false;
}

void NetPrefetcher::abort() {
    running_ = false;
    timeoutTimer_->stop();
    if (reply_) {
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    if (ring_) {
        ring_->abort();
    }
    LOG_I("NetPrefetcher aborted");
}

double NetPrefetcher::bufferRatio() const {
    if (!ring_) return 0.0;
    return static_cast<double>(ring_->available()) / ring_->freeSpace();
}

void NetPrefetcher::onReadyRead() {
    if (paused_ || !running_ || !ring_) return;

    hasData_ = true;
    timeoutTimer_->start(); // 重置计时器

    QByteArray data = reply_->readAll();
    size_t written = ring_->write(
        reinterpret_cast<const uint8_t*>(data.constData()), data.size());
    if (written < static_cast<size_t>(data.size())) {
        // 缓冲满，暂停
        emit bufferingChanged(true);
    }
    emit downloadProgress(bufferRatio());
}

void NetPrefetcher::onFinished() {
    LOG_I("NetPrefetcher finished downloading");
    running_ = false;
    timeoutTimer_->stop();
    emit downloadProgress(1.0);
}

void NetPrefetcher::onDownloadProgress(qint64 received, qint64 total) {
    if (total > 0) {
        progress_.store(static_cast<double>(received) / total);
    }
}

void NetPrefetcher::onError(QNetworkReply::NetworkError code) {
    LOG_E("Network error: %d", code);
    running_ = false;
    timeoutTimer_->stop();
}