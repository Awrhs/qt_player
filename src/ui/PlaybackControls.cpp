#include "PlaybackControls.h"
#include "../utils/Log.h"

#include <cmath>

LOG_TAG("Controls")

PlaybackControls::PlaybackControls(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    // ── 播放/暂停 ──
    btnPlayPause_ = new QPushButton(QStringLiteral(">"), this);
    btnPlayPause_->setFixedSize(32, 32);
    btnPlayPause_->setToolTip(tr("播放/暂停 (Space)"));
    connect(btnPlayPause_, &QPushButton::clicked, this, &PlaybackControls::playPauseClicked);

    // ── 逐帧后退 ──
    btnStepBack_ = new QPushButton(QStringLiteral("|<"), this);
    btnStepBack_->setFixedSize(32, 32);
    btnStepBack_->setToolTip(tr("逐帧后退"));
    btnStepBack_->setEnabled(false);
    connect(btnStepBack_, &QPushButton::clicked, this, &PlaybackControls::stepBackwardClicked);

    // ── 逐帧前进 ──
    btnStepFwd_ = new QPushButton(QStringLiteral(">|"), this);
    btnStepFwd_->setFixedSize(32, 32);
    btnStepFwd_->setToolTip(tr("逐帧前进"));
    btnStepFwd_->setEnabled(false);
    connect(btnStepFwd_, &QPushButton::clicked, this, &PlaybackControls::stepForwardClicked);

    // ── 进度条 ──
    slider_ = new ClickableSlider(Qt::Horizontal, this);
    slider_->setRange(0, 1000);
    slider_->setValue(0);
    connect(slider_, &QSlider::sliderPressed,  this, &PlaybackControls::onSliderPressed);
    connect(slider_, &QSlider::sliderReleased, this, &PlaybackControls::onSliderReleased);
    connect(slider_, &QSlider::sliderMoved,    this, &PlaybackControls::onSliderMoved);

    // ── 时间标签 ──
    labelTime_ = new QLabel(QStringLiteral("00:00 / 00:00"), this);
    labelTime_->setMinimumWidth(100);
    labelTime_->setAlignment(Qt::AlignCenter);

    // ── 倍速 ──
    btnSpeed_ = new QPushButton(QStringLiteral("1.0x"), this);
    btnSpeed_->setFixedWidth(48);
    btnSpeed_->setToolTip(tr("切换倍速"));
    btnSpeed_->setCheckable(false);
    connect(btnSpeed_, &QPushButton::clicked, this, &PlaybackControls::onSpeedCycle);

    // ── 音量 ──
    btnMute_ = new QPushButton(QStringLiteral("Vol"), this);
    btnMute_->setFixedSize(32, 32);
    btnMute_->setToolTip(tr("静音"));
    btnMute_->setCheckable(true);
    connect(btnMute_, &QPushButton::toggled, this, &PlaybackControls::muteToggled);
    // 静音状态切换文本
    connect(btnMute_, &QPushButton::toggled, this, [this](bool muted) {
        btnMute_->setText(muted ? QStringLiteral("Mute") : QStringLiteral("Vol"));
    });

    volumeSlider_ = new ClickableSlider(Qt::Horizontal, this);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(100);
    volumeSlider_->setFixedWidth(80);
    connect(volumeSlider_, &QSlider::valueChanged, this, &PlaybackControls::onVolumeSliderChanged);

    // ── 组装 ──
    layout->addWidget(btnPlayPause_);
    layout->addWidget(btnStepBack_);
    layout->addWidget(btnStepFwd_);
    layout->addWidget(slider_, 1);
    layout->addWidget(labelTime_);
    layout->addWidget(btnSpeed_);
    layout->addWidget(btnMute_);
    layout->addWidget(volumeSlider_);
}

void PlaybackControls::updatePosition(double current, double duration) {
    if (duration > 0) duration_ = duration;   // 同步时长，供 onSliderReleased 换算

    if (seeking_) return;

    // seek 已发出但时钟尚未跳到目标位置前，不要用旧时钟值把滑块拉回
    if (pendingSeekPos_ >= 0.0 && std::fabs(current - pendingSeekPos_) > 0.5) {
        return;
    }
    pendingSeekPos_ = -1.0;   // 时钟已追上目标，恢复跟随

    if (duration > 0) {
        slider_->setValue(static_cast<int>(current / duration * 1000));
    }

    auto fmt = [](double sec) -> QString {
        if (sec < 0) sec = 0;
        int total = static_cast<int>(sec);
        int m = total / 60;
        int s = total % 60;
        return QStringLiteral("%1:%2")
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    };

    labelTime_->setText(QStringLiteral("%1 / %2").arg(fmt(current), fmt(duration)));
}

void PlaybackControls::setPlaying(bool playing) {
    btnPlayPause_->setText(playing ? QStringLiteral("||") : QStringLiteral(">"));
}

void PlaybackControls::setDuration(double seconds) {
    duration_ = seconds;
}

void PlaybackControls::setSpeedText(const QString& text) {
    btnSpeed_->setText(text);
}

void PlaybackControls::setStepEnabled(bool enabled) {
    btnStepBack_->setEnabled(enabled);
    btnStepFwd_->setEnabled(enabled);
}

void PlaybackControls::onSliderPressed() {
    seeking_ = true;
    LOG_I("onSliderPressed");
}

void PlaybackControls::onSliderReleased() {
    seeking_ = false;
    // 滑块值 0~1000 是全时长的千分比，换算成秒
    double fraction = slider_->value() / 1000.0;
    double pos = fraction * duration_;
    pendingSeekPos_ = pos;
    LOG_I("onSliderReleased: slider=%d (%.1f%%), pos=%.3fs, duration=%.2fs",
          slider_->value(), fraction * 100.0, pos, duration_);
    if (duration_ > 0) {
        emit seekRequested(pos);
    } else {
        LOG_W("onSliderReleased: duration unknown, seek ignored");
    }
}

void PlaybackControls::onSliderMoved(int value) {
    Q_UNUSED(value)
}

void PlaybackControls::onSpeedCycle() {
    speedIndex_ = (speedIndex_ + 1) % speedPresets_.size();
    double speed = speedPresets_[speedIndex_];
    btnSpeed_->setText(QStringLiteral("%1x").arg(speed, 0, 'f', 2));
    emit speedChanged(speed);
}

void PlaybackControls::onVolumeSliderChanged(int value) {
    emit volumeChanged(value);
}
