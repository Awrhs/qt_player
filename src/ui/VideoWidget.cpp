#include "VideoWidget.h"

#include <QPainter>
#include <QResizeEvent>
#include <QStyleOption>

VideoWidget::VideoWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setStyleSheet("background-color: black;");
}

void VideoWidget::setFrame(const QImage& image) {
    {
        QMutexLocker lock(&frameMtx_);
        currentFrame_ = image;
        hasFrame_ = true;
    }
    // 首帧（或分辨率变化）会改变宽高比，重新计算并下发输出尺寸
    updateTargetSize();
    // 触发异步重绘
    update();
}

void VideoWidget::clearFrame() {
    QMutexLocker lock(&frameMtx_);
    currentFrame_ = {};
    hasFrame_ = false;
    update();
}

QSizeF VideoWidget::logicalSizeLocked() const {
    if (!hasFrame_ || currentFrame_.isNull()) return {};
    // displayAspect_ 已含 SAR：保持像素高度、按显示比例调整逻辑宽度
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
    // 比例变化改变适配尺寸，立即重新下发
    lastEmittedSize_ = {};
    updateTargetSize();
    update();
}

void VideoWidget::resizeEvent(QResizeEvent* /*event*/) {
    updateTargetSize();
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

    // 背景
    painter.fillRect(rect(), Qt::black);

    QMutexLocker lock(&frameMtx_);

    if (!hasFrame_ || currentFrame_.isNull()) {
        // 空状态：显示提示文字
        painter.setPen(Qt::gray);
        QFont font = painter.font();
        font.setPointSize(14);
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, tr("播放器已就绪\n打开文件开始播放"));
        return;
    }

    // 按显示宽高比（含 SAR）计算目标矩形（与上报给播放器的输出尺寸一致）
    QSizeF  scaledSize = logicalSizeLocked().scaled(QSizeF(size()),
                                                    Qt::KeepAspectRatio);
    QRectF  targetRect = QRectF(
        (width()  - scaledSize.width())  / 2.0,
        (height() - scaledSize.height()) / 2.0,
        scaledSize.width(), scaledSize.height());

    // 正常情况下 currentFrame_ 已由 sws 缩放到目标尺寸，这里是 1:1 拷贝；
    // 若尺寸暂时不一致（如窗口刚拖动、首帧未及重配），drawImage 按目标矩形
    // 缩放兜底。默认不开 SmoothPixmapTransform（高帧率下软件平滑缩放会超出
    // 帧预算）；尺寸一致时该提示无影响。
    painter.drawImage(targetRect, currentFrame_);
}
