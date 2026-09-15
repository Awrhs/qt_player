#include "EqualizerPanel.h"
#include <QStyle>

EqualizerPanel::EqualizerPanel(Equalizer* equalizer, QWidget* parent)
    : QWidget(parent), equalizer_(equalizer)
{
    // 水平布局：10 组推子 + 预设
    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(8, 4, 8, 4);
    mainLayout->setSpacing(4);

    for (int i = 0; i < 10; ++i) {
        auto* bandLayout = new QVBoxLayout();
        bandLayout->setSpacing(2);
        bandLayout->setAlignment(Qt::AlignCenter);

        // 增益值标签
        auto* valLabel = new QLabel(QStringLiteral("0"), this);
        valLabel->setAlignment(Qt::AlignCenter);
        valLabel->setFixedWidth(32);
        valLabel->setStyleSheet("font-size: 9px;");
        valueLabels_[i] = valLabel;

        // 垂直推子
        auto* slider = new QSlider(Qt::Vertical, this);
        slider->setRange(-120, 120); // -12.0dB ~ +12.0dB (×10)
        slider->setValue(0);
        slider->setFixedHeight(80);
        slider->setTickPosition(QSlider::NoTicks);
        sliders_[i] = slider;

        // 频率标签
        auto* freqLabel = new QLabel(FREQ_LABELS[i], this);
        freqLabel->setAlignment(Qt::AlignCenter);
        freqLabel->setStyleSheet("font-size: 9px; color: #888;");

        bandLayout->addWidget(valLabel);
        bandLayout->addWidget(slider);
        bandLayout->addWidget(freqLabel);

        mainLayout->addLayout(bandLayout);

        // 信号连接
        connect(slider, &QSlider::valueChanged, this, [this, i](int value) {
            onSliderChanged(i, value);
        });
    }

    // 预设下拉框
    auto* presetLayout = new QVBoxLayout();
    presetCombo_ = new QComboBox(this);
    for (const auto& name : Equalizer::presetNames()) {
        presetCombo_->addItem(QString::fromStdString(name));
    }
    presetCombo_->setCurrentText("Flat");
    connect(presetCombo_, &QComboBox::currentTextChanged,
            this, &EqualizerPanel::onPresetSelected);

    QLabel* presetLabel = new QLabel(tr("预设"), this);
    presetLabel->setAlignment(Qt::AlignCenter);
    presetLabel->setStyleSheet("font-size: 9px; color: #888;");
    presetLayout->addWidget(presetLabel);
    presetLayout->addWidget(presetCombo_);

    mainLayout->addSpacing(8);
    mainLayout->addLayout(presetLayout);
}

void EqualizerPanel::onSliderChanged(int band, int value) {
    double dB = value / 10.0;
    valueLabels_[band]->setText(QStringLiteral("%1").arg(dB, 0, 'f', 1));
    if (equalizer_) {
        equalizer_->setBandGain(band, dB);
    }
    emit bandGainChanged(band, dB);
}

void EqualizerPanel::onPresetSelected(const QString& name) {
    if (equalizer_) {
        equalizer_->setPreset(name.toStdString());
        // 更新所有推子位置
        for (int i = 0; i < 10; ++i) {
            double gain = equalizer_->getBandGain(i);
            int val = static_cast<int>(gain * 10);
            sliders_[i]->setValue(val);
            valueLabels_[i]->setText(QStringLiteral("%1").arg(gain, 0, 'f', 1));
        }
    }
    emit presetChanged(name);
}