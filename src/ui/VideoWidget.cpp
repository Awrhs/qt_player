#include "VideoWidget.h"

#include <QPainter>
#include <QStyleOption>

VideoWidget::VideoWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setStyleSheet("background-color: black;");
}

void VideoWidget::setFrame(const QImage& image) {
    QMutexLocker lock(&frameMtx_);
    currentFrame_ = image;
    hasFrame_ = true;
    // 触发异步重绘
    update();
}

void VideoWidget::clearFrame() {
    QMutexLocker lock(&frameMtx_);
    currentFrame_ = {};
    hasFrame_ = false;
    update();
}

void VideoWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

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

    // 保持宽高比缩放
    QImage scaled = currentFrame_.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    int x = (width() - scaled.width()) / 2;
    int y = (height() - scaled.height()) / 2;
    painter.drawImage(x, y, scaled);
}