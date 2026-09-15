#include "SpectrumWidget.h"
#include <QPainter>
#include <cmath>

SpectrumWidget::SpectrumWidget(QWidget* parent)
    : QWidget(parent), bands_(24, 0.0f)
{
    setMinimumSize(200, 60);
    setFixedHeight(80);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    timer_ = new QTimer(this);
    timer_->setInterval(33); // ~30fps
    connect(timer_, &QTimer::timeout, this, &SpectrumWidget::onTick);
}

void SpectrumWidget::setAnalyzer(SpectrumAnalyzer* analyzer) {
    analyzer_ = analyzer;
}

void SpectrumWidget::start(bool on) {
    active_ = on;
    if (on) {
        timer_->start();
    } else {
        timer_->stop();
        bands_.assign(24, 0.0f);
        update();
    }
}

void SpectrumWidget::onTick() {
    if (analyzer_) {
        bands_ = analyzer_->bands();
        update();
    }
}

void SpectrumWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // 背景
    painter.fillRect(rect(), QColor(20, 20, 30));

    if (!active_ || bands_.empty()) {
        painter.setPen(QColor(100, 100, 120));
        painter.drawText(rect(), Qt::AlignCenter, tr("频谱"));
        return;
    }

    int w = width();
    int h = height();
    int numBands = std::min(24, static_cast<int>(bands_.size()));
    float barWidth = static_cast<float>(w) / numBands;
    float margin = barWidth * 0.15f;

    for (int i = 0; i < numBands; ++i) {
        float val = std::min(1.0f, std::max(0.0f, bands_[i]));
        float barH = val * (h - 8);
        float x = i * barWidth + margin / 2;
        float y = h - barH - 4;

        // 颜色渐变：低频→蓝绿 (0,200,200) 中频→黄 (255,255,0) 高频→红 (255,0,0)
        float t = static_cast<float>(i) / numBands;
        int r, g, b;
        if (t < 0.33f) {
            // 蓝→绿
            float p = t / 0.33f;
            r = 0;
            g = static_cast<int>(100 + 155 * p);
            b = static_cast<int>(180 - 80 * p);
        } else if (t < 0.66f) {
            // 绿→黄
            float p = (t - 0.33f) / 0.33f;
            r = static_cast<int>(100 * p);
            g = 255;
            b = static_cast<int>(100 - 100 * p);
        } else {
            // 黄→红
            float p = (t - 0.66f) / 0.34f;
            r = static_cast<int>(255);
            g = static_cast<int>(255 - 200 * p);
            b = 0;
        }

        QColor color(r, g, b);
        painter.fillRect(QRectF(x, y, barWidth - margin, barH), color);
    }
}