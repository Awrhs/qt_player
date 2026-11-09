#include "VideoWidget.h"

#include <QPainter>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QStyleOption>

VideoWidget::VideoWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setStyleSheet("background-color: black;");
    setFocusPolicy(Qt::NoFocus);
    placeholder_ = tr("播放器已就绪\n打开文件或拖入文件开始播放");
}

void VideoWidget::setFrame(const QImage& image) {
    {
        QMutexLocker lock(&frameMtx_);
        currentFrame_ = image;
        hasFrame_ = true;
    }
    updateTargetSize();
    update();
}

void VideoWidget::clearFrame() {
    {
        QMutexLocker lock(&frameMtx_);
        currentFrame_ = {};
        hasFrame_ = false;
    }
    update();
}

void VideoWidget::setPlaceholderText(const QString& text) {
    placeholder_ = text;
    update();
}

QSizeF VideoWidget::logicalSizeLocked() const {
    if (!hasFrame_ || currentFrame_.isNull()) return {};
    if (displayAspect_ > 0.0) {
        return QSizeF(currentFrame_.height() * displayAspect_,
                      currentFrame_.height());
    }
    return QSizeF(currentFrame_.size());
}

QSize VideoWidget::contentSize() const {
    QMutexLocker lock(&frameMtx_);
    QSizeF logical = logicalSizeLocked();
    if (logical.isEmpty()) return {};
    return logical.scaled(QSizeF(size()), Qt::KeepAspectRatio).toSize();
}

void VideoWidget::setDisplayAspect(double aspect) {
    {
        QMutexLocker lock(&frameMtx_);
        displayAspect_ = aspect;
    }
    lastEmittedSize_ = {};
    updateTargetSize();
    update();
}

void VideoWidget::resizeEvent(QResizeEvent* /*event*/) {
    updateTargetSize();
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit doubleClicked();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void VideoWidget::updateTargetSize() {
    QSize target = contentSize();
    if (!target.isEmpty() && target != lastEmittedSize_) {
        lastEmittedSize_ = target;
        emit contentResolutionChanged(target.width(), target.height());
    }
}

void VideoWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    QMutexLocker lock(&frameMtx_);

    if (!hasFrame_ || currentFrame_.isNull()) {
        painter.setPen(Qt::gray);
        QFont font = painter.font();
        font.setPointSize(14);
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, placeholder_);
        return;
    }

    QSizeF  scaledSize = logicalSizeLocked().scaled(QSizeF(size()),
                                                    Qt::KeepAspectRatio);
    QRectF  targetRect = QRectF(
        (width()  - scaledSize.width())  / 2.0,
        (height() - scaledSize.height()) / 2.0,
        scaledSize.width(), scaledSize.height());

    // 正常情况下 currentFrame_ 已由 sws 缩放到目标尺寸，这里是 1:1 拷贝；
    // 尺寸暂时不一致（窗口刚拖动）时 drawImage 按目标矩形缩放兜底。
    painter.drawImage(targetRect, currentFrame_);
}
