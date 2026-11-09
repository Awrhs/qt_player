#ifndef PLAYBACK_CONTROLS_H
#define PLAYBACK_CONTROLS_H

#include <QWidget>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "ClickableSlider.h"

/**
 * @brief 播放控制栏（底部）
 *
 * 播放/暂停、停止、逐帧、进度条（拖动时预览时间）、时间显示、倍速、循环、音量/静音。
 * 所有子控件不获取键盘焦点，快捷键统一由主窗口处理。
 */
class PlaybackControls : public QWidget {
    Q_OBJECT
public:
    explicit PlaybackControls(QWidget* parent = nullptr);

    void updatePosition(double current, double duration);
    void setPlaying(bool playing);
    void setDuration(double seconds);
    void setSpeed(double speed);
    void setStepEnabled(bool enabled);
    void setLoop(bool on);
    void setVolume(int percent);
    void setMuted(bool muted);
    int  volume() const;
    bool isMuted() const;

    /** @brief mm:ss，超过 1 小时为 h:mm:ss */
    static QString formatTime(double seconds);

signals:
    void playPauseClicked();
    void stopClicked();
    void seekRequested(double position);
    void speedCycleRequested();
    void stepForwardClicked();
    void stepBackwardClicked();
    void volumeChanged(int percent);
    void muteToggled(bool muted);
    void loopToggled(bool on);

private slots:
    void onSliderPressed();
    void onSliderReleased();
    void onSliderMoved(int value);

private:
    void updateTimeLabel(double current);

    QPushButton*     btnPlayPause_;
    QPushButton*     btnStop_;
    QPushButton*     btnStepBack_;
    QPushButton*     btnStepFwd_;
    ClickableSlider* slider_;
    QLabel*          labelTime_;
    QPushButton*     btnSpeed_;
    QPushButton*     btnLoop_;
    QPushButton*     btnMute_;
    ClickableSlider* volumeSlider_;

    bool   seeking_  = false;
    double duration_ = 0.0;
    double current_  = 0.0;

    static constexpr int kSliderMax = 10000;
};

#endif // PLAYBACK_CONTROLS_H
