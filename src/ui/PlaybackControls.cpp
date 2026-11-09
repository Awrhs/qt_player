#include "PlaybackControls.h"
#include "../utils/Log.h"

#include <cmath>
#include <algorithm>

LOG_TAG("Controls")

PlaybackControls::PlaybackControls(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    auto makeButton = [this](const QString& text, const QString& tip, int width = 32) {
        auto* b = new QPushButton(text, this);
        b->setFixedSize(width, 32);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };

    btnPlayPause_ = makeButton(QStringLiteral(">"), tr("播放/暂停 (Space)"));
    connect(btnPlayPause_, &QPushButton::clicked, this, &PlaybackControls::playPauseClicked);

    btnStop_ = makeButton(QStringLiteral("■"), tr("停止 (S)"));
    connect(btnStop_, &QPushButton::clicked, this, &PlaybackControls::stopClicked);

    btnStepBack_ = makeButton(QStringLiteral("|<"), tr("上一帧 (,)"));
    btnStepBack_->setEnabled(false);
    connect(btnStepBack_, &QPushButton::clicked, this, &PlaybackControls::stepBackwardClicked);

    btnStepFwd_ = makeButton(QStringLiteral(">|"), tr("下一帧 (.)"));
    btnStepFwd_->setEnabled(false);
    connect(btnStepFwd_, &QPushButton::clicked, this, &PlaybackControls::stepForwardClicked);

    slider_ = new ClickableSlider(Qt::Horizontal, this);
    slider_->setRange(0, kSliderMax);
    slider_->setValue(0);
    slider_->setFocusPolicy(Qt::NoFocus);
    connect(slider_, &QSlider::sliderPressed,  this, &PlaybackControls::onSliderPressed);
    connect(slider_, &QSlider::sliderReleased, this, &PlaybackControls::onSliderReleased);
    connect(slider_, &QSlider::sliderMoved,    this, &PlaybackControls::onSliderMoved);

    labelTime_ = new QLabel(QStringLiteral("00:00 / 00:00"), this);
    labelTime_->setMinimumWidth(110);
    labelTime_->setAlignment(Qt::AlignCenter);

    btnSpeed_ = makeButton(QStringLiteral("1.0x"), tr("切换倍速 ([ / ] 调节，Backspace 复位)"), 52);
    connect(btnSpeed_, &QPushButton::clicked, this, &PlaybackControls::speedCycleRequested);

    btnLoop_ = makeButton(QStringLiteral("↻"), tr("循环播放 (L)"));
    btnLoop_->setCheckable(true);
    connect(btnLoop_, &QPushButton::toggled, this, &PlaybackControls::loopToggled);

    btnMute_ = makeButton(QStringLiteral("Vol"), tr("静音 (M)"));
    btnMute_->setCheckable(true);
    connect(btnMute_, &QPushButton::toggled, this, &PlaybackControls::muteToggled);
    connect(btnMute_, &QPushButton::toggled, this, [this](bool muted) {
        btnMute_->setText(muted ? QStringLiteral("Mute") : QStringLiteral("Vol"));
    });

    volumeSlider_ = new ClickableSlider(Qt::Horizontal, this);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(100);
    volumeSlider_->setFixedWidth(80);
    volumeSlider_->setFocusPolicy(Qt::NoFocus);
    volumeSlider_->setToolTip(tr("音量 (↑/↓)"));
    connect(volumeSlider_, &QSlider::valueChanged, this, &PlaybackControls::volumeChanged);

    layout->addWidget(btnPlayPause_);
    layout->addWidget(btnStop_);
    layout->addWidget(btnStepBack_);
    layout->addWidget(btnStepFwd_);
    layout->addWidget(slider_, 1);
    layout->addWidget(labelTime_);
    layout->addWidget(btnSpeed_);
    layout->addWidget(btnLoop_);
    layout->addWidget(btnMute_);
    layout->addWidget(volumeSlider_);

    setFocusPolicy(Qt::NoFocus);
}

QString PlaybackControls::formatTime(double seconds) {
    if (seconds < 0 || !std::isfinite(seconds)) seconds = 0;
    const int total = static_cast<int>(seconds + 0.0005);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

void PlaybackControls::updateTimeLabel(double current) {
    labelTime_->setText(QStringLiteral("%1 / %2")
                            .arg(formatTime(current), formatTime(duration_)));
}

void PlaybackControls::updatePosition(double current, double duration) {
    if (duration > 0) duration_ = duration;
    current_ = current;
    if (seeking_) return;   // 拖动中：滑块与标签跟随鼠标，不被时钟拉回

    if (duration_ > 0) {
        slider_->setValue(static_cast<int>(std::lround(current / duration_ * kSliderMax)));
    } else {
        slider_->setValue(0);
    }
    updateTimeLabel(current);
}

void PlaybackControls::setPlaying(bool playing) {
    btnPlayPause_->setText(playing ? QStringLiteral("||") : QStringLiteral(">"));
}

void PlaybackControls::setDuration(double seconds) {
    duration_ = seconds;
    if (!seeking_) updateTimeLabel(current_);
}

void PlaybackControls::setSpeed(double speed) {
    btnSpeed_->setText(QStringLiteral("%1x").arg(speed, 0, 'g', 3));
}

void PlaybackControls::setStepEnabled(bool enabled) {
    btnStepBack_->setEnabled(enabled);
    btnStepFwd_->setEnabled(enabled);
}

void PlaybackControls::setLoop(bool on) {
    if (btnLoop_->isChecked() == on) return;
    QSignalBlocker blocker(btnLoop_);
    btnLoop_->setChecked(on);
}

void PlaybackControls::setVolume(int percent) {
    volumeSlider_->setValue(std::clamp(percent, 0, 100));
}

void PlaybackControls::setMuted(bool muted) {
    btnMute_->setChecked(muted);
}

int PlaybackControls::volume() const {
    return volumeSlider_->value();
}

bool PlaybackControls::isMuted() const {
    return btnMute_->isChecked();
}

void PlaybackControls::onSliderPressed() {
    seeking_ = true;
}

void PlaybackControls::onSliderReleased() {
    seeking_ = false;
    const double fraction = slider_->value() / static_cast<double>(kSliderMax);
    const double pos = fraction * duration_;
    LOG_I("seek from slider: %.1f%% -> %.3fs (duration %.2fs)", fraction * 100.0, pos, duration_);
    if (duration_ > 0) {
        emit seekRequested(pos);
    } else {
        LOG_W("duration unknown, seek ignored");
    }
}

void PlaybackControls::onSliderMoved(int value) {
    // 拖动预览：时间标签实时显示目标位置
    if (duration_ > 0) {
        updateTimeLabel(value / static_cast<double>(kSliderMax) * duration_);
    }
}
