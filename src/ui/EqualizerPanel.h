#ifndef EQUALIZER_PANEL_H
#define EQUALIZER_PANEL_H

#include <QWidget>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <vector>
#include <array>

#include "../core/Equalizer.h"

/**
 * @brief 均衡器面板
 *
 * 10 个垂直推子 + 频率标签 + 预设下拉框
 */
class EqualizerPanel : public QWidget {
    Q_OBJECT
public:
    explicit EqualizerPanel(Equalizer* equalizer, QWidget* parent = nullptr);

signals:
    void bandGainChanged(int band, double dB);
    void presetChanged(const QString& name);

private slots:
    void onSliderChanged(int band, int value);
    void onPresetSelected(const QString& name);

private:
    Equalizer* equalizer_ = nullptr;
    std::array<QSlider*, 10> sliders_;
    std::array<QLabel*, 10> valueLabels_;
    QComboBox* presetCombo_ = nullptr;

    // ISO 频率标签
    static constexpr const char* FREQ_LABELS[10] = {
        "31", "62", "125", "250", "500",
        "1k", "2k", "4k", "8k", "16k"
    };
};

#endif // EQUALIZER_PANEL_H