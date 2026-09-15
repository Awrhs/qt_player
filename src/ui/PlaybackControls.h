#ifndef PLAYBACK_CONTROLS_H
#define PLAYBACK_CONTROLS_H

#include <QWidget>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <vector>

/**
 * @brief 播放控制栏（底部）
 *
 * 包含：播放/暂停、进度条、时间显示、倍速、逐帧、音量。
 */
class PlaybackControls : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackControls(QWidget* parent = nullptr);

    void updatePosition(double current, double duration);
    void setPlaying(bool playing);
    void setDuration(double seconds);
    void setSpeedText(const QString& text);
    void setStepEnabled(bool enabled);

signals:
    void playPauseClicked();
    void seekRequested(double position);
    void speedChanged(double factor);
    void stepForwardClicked();
    void stepBackwardClicked();
    void volumeChanged(int percent);
    void muteToggled(bool muted);

private slots:
    void onSliderPressed();
    void onSliderReleased();
    void onSliderMoved(int value);
    void onSpeedCycle();
    void onVolumeSliderChanged(int value);

private:
    QPushButton* btnPlayPause_;
    QSlider*     slider_;
    QLabel*      labelTime_;
    QPushButton* btnStepBack_;
    QPushButton* btnStepFwd_;
    QPushButton* btnSpeed_;
    QSlider*     volumeSlider_;
    QPushButton* btnMute_;
    bool         seeking_ = false;

    // 倍速预设
    std::vector<double> speedPresets_ = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
    int                speedIndex_    = 2; // 默认 1.0x
};

#endif // PLAYBACK_CONTROLS_H