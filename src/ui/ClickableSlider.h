#ifndef CLICKABLE_SLIDER_H
#define CLICKABLE_SLIDER_H

#include <QSlider>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>

/**
 * @brief 支持点击轨道直接跳转的 QSlider
 *
 * 左键按下/拖动时，滑块值精确跟随鼠标在 groove 中的位置（用 QStyle 的
 * subControlRect 计算，扣除 handle 半宽，点击两端不再有系统偏差），并通过
 * 基类信号 sliderPressed / sliderMoved / sliderReleased 通知外部。
 *
 * 与基类的差异仅在左键：不做 pageStep 翻页、不做按住重复；键盘、滚轮、
 * 右键等仍走基类。setSliderDown 与基类状态保持一致（isSliderDown/
 * State_Sunken 不失配）。水平、垂直方向均可用，垂直方向遵循
 * invertedAppearance。
 */
class ClickableSlider : public QSlider {
public:
    explicit ClickableSlider(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QSlider(orientation, parent) {}

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (!isEnabled()) {
            QSlider::mousePressEvent(event);
            return;
        }
        if (event->button() == Qt::LeftButton) {
            dragging_ = true;
            int v = valueFromPos(event->position().toPoint());
            if (v != value()) {
                setValue(v);
                emit sliderMoved(v);
            }
            // setSliderDown(true) 内部会发 sliderPressed()，不可再手动 emit，
            // 否则外部会收到两次 pressed/released（seek 被执行两遍）
            setSliderDown(true);
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_) {
            int v = valueFromPos(event->position().toPoint());
            if (v != value()) {
                setValue(v);
                emit sliderMoved(v);
            }
            event->accept();
            return;
        }
        QSlider::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && dragging_) {
            dragging_ = false;
            // setSliderDown(false) 内部会发 sliderReleased()
            setSliderDown(false);
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

private:
    /** 用 QStyle 把鼠标像素位置精确映射为滑块值（扣除 groove/handle 边距） */
    int valueFromPos(const QPoint& pos) const {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QStyle* st = style();
        QRect groove = st->subControlRect(QStyle::CC_Slider, &opt,
                                          QStyle::SC_SliderGroove, this);
        QRect handle = st->subControlRect(QStyle::CC_Slider, &opt,
                                          QStyle::SC_SliderHandle, this);

        if (orientation() == Qt::Horizontal) {
            int span = groove.width() - handle.width();
            int p = pos.x() - groove.left() - handle.width() / 2;
            return QStyle::sliderValueFromPosition(
                minimum(), maximum(), p, span, false);
        } else {
            int span = groove.height() - handle.height();
            int p = pos.y() - groove.top() - handle.height() / 2;
            // 垂直滑块默认底部为 minimum、顶部为 maximum；invertedAppearance 反转
            bool upsideDown = !invertedAppearance();
            return QStyle::sliderValueFromPosition(
                minimum(), maximum(), p, span, upsideDown);
        }
    }

    bool dragging_ = false;
};

#endif // CLICKABLE_SLIDER_H
