#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QImage>
#include <QTimer>
#include <memory>

#include "core/PlayerController.h" // PlayState 定义

class VideoWidget;
class PlaybackControls;
class SpectrumWidget;
class EqualizerPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

public slots:
    void openFile();
    void openPath(const QString& path);

private slots:
    void onPlayPause();
    void onSeek(double position);
    void onSpeedChanged(double factor);
    void onVolumeChanged(int percent);
    void onMuteToggled(bool muted);
    void onStateChanged(PlayState state);
    void onFrameReady(const QImage& image);
    void onError(const QString& msg);
    void onPositionUpdate();
    void onPlaybackFinished();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setupMenuBar();
    void setupUI();

    PlayerController*   controller_     = nullptr;
    VideoWidget*        videoWidget_    = nullptr;
    PlaybackControls*   controls_       = nullptr;
    SpectrumWidget*     spectrumWidget_ = nullptr;
    EqualizerPanel*     eqPanel_        = nullptr;
    QTimer*             positionTimer_  = nullptr;
};

#endif // MAIN_WINDOW_H