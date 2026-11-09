#ifndef VIDEO_WIDGET_H
#define VIDEO_WIDGET_H

#include <QWidget>
#include <QImage>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QMutex>

/**
 * @brief 视频帧显示控件
 *
 * 在 QWidget 上绘制视频帧，保持宽高比自适应。
 * 线程安全：setFrame 可在任意线程调用（内部用 mutex 保护）。
 *
 * 性能约定：播放器会按 contentSize() 下发的目标像素尺寸让 sws 直接缩放到
 * 窗口内容大小，paintEvent 只做 1:1 绘制，避免每帧在 UI 线程做全分辨率
 * 平滑缩放。
 */
class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);

    /** @brief 设置要显示的帧（线程安全） */
    void setFrame(const QImage& image);

    /** @brief 清空显示，回到黑色背景+提示文字 */
    void clearFrame();

    /** @brief 无画面时显示的提示文字（如纯音频文件名） */
    void setPlaceholderText(const QString& text);

    /** @brief 当前控件尺寸下、按显示宽高比适配出的内容像素尺寸 */
    QSize contentSize() const;

    /** @brief 设置显示宽高比（已含 SAR 修正，= 宽/高）；<0 回退到帧像素宽高比 */
    void setDisplayAspect(double aspect);

signals:
    /** @brief 期望的视频输出像素尺寸发生变化（窗口尺寸变化或首帧到达） */
    void contentResolutionChanged(int width, int height);
    /** @brief 画面区域被双击（切换全屏） */
    void doubleClicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QSizeF logicalSizeLocked() const;
    void updateTargetSize();

    QImage  currentFrame_;
    mutable QMutex  frameMtx_;
    bool    hasFrame_ = false;
    QSize   lastEmittedSize_;
    double  displayAspect_ = -1.0;
    QString placeholder_;
};

#endif // VIDEO_WIDGET_H
