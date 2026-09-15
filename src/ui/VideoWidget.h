#ifndef VIDEO_WIDGET_H
#define VIDEO_WIDGET_H

#include <QWidget>
#include <QImage>
#include <QPixmap>
#include <QMutex>

/**
 * @brief 视频帧显示控件
 *
 * 在 QWidget 上绘制视频帧，保持宽高比自适应。
 * 线程安全：setFrame 可在任意线程调用（内部用 mutex 保护）。
 */
class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);

    /**
     * @brief 设置要显示的帧（线程安全）
     * @param image RGBA QImage
     */
    void setFrame(const QImage& image);

    /** @brief 清空显示，回到黑色背景+提示文字 */
    void clearFrame();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage  currentFrame_;
    QMutex  frameMtx_;
    bool    hasFrame_ = false;
};

#endif // VIDEO_WIDGET_H