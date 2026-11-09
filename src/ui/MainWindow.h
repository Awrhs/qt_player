#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QImage>
#include <QTimer>
#include <QString>
#include <QList>
#include <QKeySequence>
#include <vector>
#include <functional>

#include "core/PlayerController.h" // PlayState 定义
#include "utils/ResumeManager.h"

class VideoWidget;
class PlaybackControls;
class SpectrumWidget;
class EqualizerPanel;
class QAction;
class QActionGroup;
class QMenu;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

public slots:
    void openFile();
    void openUrl();
    void openPath(const QString& path);
    void toggleFullScreen();
    void setFullScreen(bool on);

private slots:
    void onPlayPause();
    void onStop();
    void onSeek(double position);
    void onSpeedCycle();
    void onSpeedStep(int direction);
    void onVolumeChanged(int percent);
    void onMuteToggled(bool muted);
    void onLoopToggled(bool on);
    void onStateChanged(PlayState state);
    void onFrameReady(const QImage& image);
    void onError(const QString& msg);
    void onWarning(const QString& msg);
    void onPositionUpdate();
    void onPlaybackFinished();
    void onMediaOpened(const QString& path, bool hasVideo, bool hasAudio);
    void onSpeedChanged(double speed);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void setupMenuBar();
    void setupUI();
    QAction* addShortcutAction(QMenu* menu, const QString& text,
                               const QList<QKeySequence>& shortcuts,
                               const std::function<void()>& handler);
    void saveResumePoint();
    int  speedPresetIndex(double speed) const;

    PlayerController*   controller_     = nullptr;
    VideoWidget*        videoWidget_    = nullptr;
    PlaybackControls*   controls_       = nullptr;
    SpectrumWidget*     spectrumWidget_ = nullptr;
    EqualizerPanel*     eqPanel_        = nullptr;
    QTimer*             positionTimer_  = nullptr;
    ResumeManager       resume_;

    QAction*      loopAction_       = nullptr;
    QAction*      muteAction_       = nullptr;
    QAction*      fullScreenAction_ = nullptr;
    QAction*      eqAction_         = nullptr;
    QAction*      specAction_       = nullptr;
    QActionGroup* speedGroup_       = nullptr;

    QString currentPath_;
    bool    fullScreen_ = false;
    Qt::WindowStates stateBeforeFullScreen_ = Qt::WindowNoState;

    std::vector<double> speedPresets_ = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
};

#endif // MAIN_WINDOW_H
