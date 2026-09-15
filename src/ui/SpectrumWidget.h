#ifndef SPECTRUM_WIDGET_H
#define SPECTRUM_WIDGET_H

#include <QWidget>
#include <QTimer>
#include <vector>

#include "../core/SpectrumAnalyzer.h"

/**
 * @brief 频谱柱状图控件
 *
 * 以 30fps 刷新率绘制 24 段频谱柱状图。
 * 颜色渐变：低频→蓝绿，中频→黄，高频→红
 */
class SpectrumWidget : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumWidget(QWidget* parent = nullptr);

    /** @brief 设置频谱分析器引用 */
    void setAnalyzer(SpectrumAnalyzer* analyzer);

    /** @brief 启动/停止刷新定时器 */
    void start(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onTick();

private:
    SpectrumAnalyzer* analyzer_ = nullptr;
    QTimer*           timer_    = nullptr;
    std::vector<float> bands_;  // 缓存的频段数据
    bool              active_   = false;
};

#endif // SPECTRUM_WIDGET_H