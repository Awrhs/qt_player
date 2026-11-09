#include "MainWindow.h"
#include "VideoWidget.h"
#include "PlaybackControls.h"
#include "SpectrumWidget.h"
#include "EqualizerPanel.h"
#include "../core/PlayerController.h"
#include "../utils/Log.h"

LOG_TAG("MainWindow")

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QStatusBar>
#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileInfo>
#include <QUrl>
#include <QSignalBlocker>
#include <cmath>
#include <algorithm>

namespace {
constexpr double kSeekStepShort = 5.0;
constexpr double kSeekStepLong  = 30.0;
constexpr int    kVolumeStep    = 5;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Qt Player"));
    resize(960, 720);
    setAcceptDrops(true);

    controller_ = new PlayerController(this);

    setupUI();
    setupMenuBar();

    positionTimer_ = new QTimer(this);
    positionTimer_->setInterval(100);
    connect(positionTimer_, &QTimer::timeout, this, &MainWindow::onPositionUpdate);

    connect(controller_, &PlayerController::frameReady,
            this, &MainWindow::onFrameReady);
    connect(controller_, &PlayerController::stateChanged,
            this, &MainWindow::onStateChanged);
    connect(controller_, &PlayerController::errorOccurred,
            this, &MainWindow::onError);
    connect(controller_, &PlayerController::warning,
            this, &MainWindow::onWarning);
    connect(controller_, &PlayerController::durationChanged,
            this, [this](double d) { controls_->setDuration(d); });
    connect(controller_, &PlayerController::positionChanged,
            this, [this](double pos) { controls_->updatePosition(pos, controller_->duration()); });
    connect(controller_, &PlayerController::playbackFinished,
            this, &MainWindow::onPlaybackFinished);
    connect(controller_, &PlayerController::displayAspectChanged,
            videoWidget_, &VideoWidget::setDisplayAspect);
    connect(controller_, &PlayerController::mediaOpened,
            this, &MainWindow::onMediaOpened);
    connect(controller_, &PlayerController::speedChanged,
            this, &MainWindow::onSpeedChanged);
    connect(controller_, &PlayerController::loopChanged, this, [this](bool on) {
        controls_->setLoop(on);
        if (loopAction_ && loopAction_->isChecked() != on) {
            QSignalBlocker b(loopAction_);
            loopAction_->setChecked(on);
        }
    });

    controls_->setSpeed(controller_->speed());
}

MainWindow::~MainWindow() = default;

// ── 窗口事件 ──────────────────────────────────────────────────────────────

void MainWindow::closeEvent(QCloseEvent* event) {
    LOG_I("closeEvent: shutting down player before close");
    saveResumePoint();
    if (controller_) {
        // 必须同步关闭：停音频设备 + join 工作线程，不能只投递异步命令
        controller_->shutdown();
    }
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty()) return;
    const QUrl& url = urls.first();
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (!path.isEmpty()) {
        openPath(path);
        event->acceptProposedAction();
    }
}

// ── 打开 ──────────────────────────────────────────────────────────────────

void MainWindow::openFile() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("打开媒体文件"), QString(),
        tr("媒体文件 (*.mp4 *.mkv *.ts *.flv *.avi *.mov *.wmv *.webm *.m4v "
           "*.mp3 *.aac *.m4a *.flac *.wav *.ogg *.opus);;所有文件 (*)"));
    if (!path.isEmpty()) {
        openPath(path);
    }
}

void MainWindow::openUrl() {
    bool ok = false;
    QString url = QInputDialog::getText(this, tr("打开 URL"), tr("媒体 URL (http/https/rtsp...)："),
                                        QLineEdit::Normal, QString(), &ok);
    url = url.trimmed();
    if (ok && !url.isEmpty()) {
        openPath(url);
    }
}

void MainWindow::openPath(const QString& path) {
    saveResumePoint();
    currentPath_ = path;

    const QString name = QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName();
    statusBar()->showMessage(tr("正在打开: %1...").arg(name));
    controller_->open(path);
    if (controller_->state() == PlayState::Idle) {
        return;   // 打开失败，错误已提示
    }

    // 渲染器随 open 重建为原始尺寸；优先用源尺寸 × 当前窗口提前配置目标，
    // 使首帧即按显示尺寸输出
    int vw = controller_->videoWidth();
    int vh = controller_->videoHeight();
    if (vw > 0 && vh > 0) {
        QSize target = QSize(vw, vh).scaled(videoWidget_->size(), Qt::KeepAspectRatio);
        controller_->setVideoTargetSize(target.width(), target.height());
    }

    // 断点续播：上次中途退出的位置
    const double dur = controller_->duration();
    if (auto rp = resume_.getResumePoint(path)) {
        if (*rp > 5.0 && dur > 0.0 && *rp < dur - 5.0) {
            controller_->seek(*rp);
            statusBar()->showMessage(tr("已从 %1 继续播放").arg(PlaybackControls::formatTime(*rp)), 4000);
        }
    }
}

void MainWindow::saveResumePoint() {
    if (currentPath_.isEmpty() || !controller_) return;
    const PlayState st = controller_->state();
    if (st == PlayState::Idle || st == PlayState::Stopped) return;
    const double pos = controller_->position();
    const double dur = controller_->duration();
    if (controller_->isEnded() || (dur > 0.0 && pos > dur - 5.0) || pos < 5.0) {
        resume_.clearResumePoint(currentPath_);
    } else {
        resume_.saveResumePoint(currentPath_, pos);
    }
}

// ── 播放控制槽 ────────────────────────────────────────────────────────────

void MainWindow::onPlayPause() {
    switch (controller_->state()) {
    case PlayState::Playing:
        controller_->pause();
        break;
    case PlayState::Paused:
        controller_->play();
        break;
    case PlayState::Stopped:
    case PlayState::Idle:
        if (!currentPath_.isEmpty()) openPath(currentPath_);
        break;
    default:
        break;
    }
}

void MainWindow::onStop() {
    saveResumePoint();
    controller_->stop();
}

void MainWindow::onSeek(double position) {
    controller_->seek(position);
}

int MainWindow::speedPresetIndex(double speed) const {
    int best = 0;
    double bestDiff = 1e9;
    for (size_t i = 0; i < speedPresets_.size(); ++i) {
        double d = std::fabs(speedPresets_[i] - speed);
        if (d < bestDiff) {
            bestDiff = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void MainWindow::onSpeedCycle() {
    int idx = (speedPresetIndex(controller_->speed()) + 1) % static_cast<int>(speedPresets_.size());
    controller_->setSpeed(speedPresets_[idx]);
}

void MainWindow::onSpeedStep(int direction) {
    int idx = speedPresetIndex(controller_->speed()) + direction;
    idx = std::clamp(idx, 0, static_cast<int>(speedPresets_.size()) - 1);
    controller_->setSpeed(speedPresets_[idx]);
}

void MainWindow::onSpeedChanged(double speed) {
    controls_->setSpeed(speed);
    if (speedGroup_) {
        for (QAction* a : speedGroup_->actions()) {
            if (std::fabs(a->data().toDouble() - speed) < 1e-6) {
                QSignalBlocker b(a);
                a->setChecked(true);
            }
        }
    }
    statusBar()->showMessage(tr("倍速 %1x").arg(speed, 0, 'g', 3), 1500);
}

void MainWindow::onVolumeChanged(int percent) {
    controller_->setVolume(percent);
}

void MainWindow::onMuteToggled(bool muted) {
    controller_->setMuted(muted);
    if (muteAction_ && muteAction_->isChecked() != muted) {
        QSignalBlocker b(muteAction_);
        muteAction_->setChecked(muted);
    }
}

void MainWindow::onLoopToggled(bool on) {
    controller_->setLoop(on);
}

void MainWindow::onStateChanged(PlayState state) {
    switch (state) {
    case PlayState::Playing:
        controls_->setPlaying(true);
        controls_->setStepEnabled(controller_->hasVideo());
        positionTimer_->start();
        spectrumWidget_->start(controller_->hasAudio());
        statusBar()->showMessage(tr("播放中"));
        break;
    case PlayState::Paused:
        controls_->setPlaying(false);
        controls_->setStepEnabled(controller_->hasVideo());
        positionTimer_->stop();
        spectrumWidget_->start(false);
        controls_->updatePosition(controller_->position(), controller_->duration());
        if (!controller_->isEnded()) statusBar()->showMessage(tr("已暂停"));
        break;
    case PlayState::Stopped:
        controls_->setPlaying(false);
        controls_->setStepEnabled(false);
        positionTimer_->stop();
        spectrumWidget_->start(false);
        controls_->setDuration(0.0);
        controls_->updatePosition(0.0, 0.0);
        videoWidget_->setPlaceholderText(tr("已停止\n按空格键重新播放"));
        videoWidget_->clearFrame();
        setWindowTitle(QStringLiteral("Qt Player"));
        statusBar()->showMessage(tr("已停止"));
        break;
    case PlayState::Idle:
        controls_->setPlaying(false);
        controls_->setStepEnabled(false);
        positionTimer_->stop();
        spectrumWidget_->start(false);
        controls_->setDuration(0.0);
        controls_->updatePosition(0.0, 0.0);
        videoWidget_->clearFrame();
        setWindowTitle(QStringLiteral("Qt Player"));
        break;
    default:
        break;
    }
}

void MainWindow::onMediaOpened(const QString& path, bool hasVideo, bool hasAudio) {
    const QString name = QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName();
    setWindowTitle(QStringLiteral("%1 - Qt Player").arg(name));
    videoWidget_->clearFrame();
    if (!hasVideo) {
        videoWidget_->setPlaceholderText(tr("正在播放音频\n%1").arg(name));
    } else {
        videoWidget_->setPlaceholderText(tr("正在加载画面..."));
    }
    Q_UNUSED(hasAudio);
}

void MainWindow::onFrameReady(const QImage& image) {
    videoWidget_->setFrame(image);
}

void MainWindow::onError(const QString& msg) {
    statusBar()->showMessage(tr("错误: %1").arg(msg));
    QMessageBox::warning(this, tr("播放器错误"), msg);
}

void MainWindow::onWarning(const QString& msg) {
    statusBar()->showMessage(msg, 5000);
}

void MainWindow::onPositionUpdate() {
    if (controller_) {
        controls_->updatePosition(controller_->position(), controller_->duration());
    }
}

void MainWindow::onPlaybackFinished() {
    if (controller_) {
        double dur = controller_->duration();
        controls_->updatePosition(dur, dur);
        if (!currentPath_.isEmpty()) resume_.clearResumePoint(currentPath_);
    }
    statusBar()->showMessage(tr("播放结束（按空格键重新播放）"));
}

// ── 全屏 ──────────────────────────────────────────────────────────────────

void MainWindow::toggleFullScreen() {
    setFullScreen(!fullScreen_);
}

void MainWindow::setFullScreen(bool on) {
    if (on == fullScreen_) return;
    fullScreen_ = on;
    if (on) {
        menuBar()->hide();
        statusBar()->hide();
        controls_->hide();
        eqPanel_->hide();
        spectrumWidget_->hide();
        showFullScreen();
    } else {
        showNormal();
        menuBar()->show();
        statusBar()->show();
        controls_->show();
        eqPanel_->setVisible(eqAction_ && eqAction_->isChecked());
        spectrumWidget_->setVisible(specAction_ && specAction_->isChecked());
    }
    if (fullScreenAction_ && fullScreenAction_->isChecked() != on) {
        QSignalBlocker b(fullScreenAction_);
        fullScreenAction_->setChecked(on);
    }
}

// ── 菜单与快捷键 ──────────────────────────────────────────────────────────

QAction* MainWindow::addShortcutAction(QMenu* menu, const QString& text,
                                       const QList<QKeySequence>& shortcuts,
                                       const std::function<void()>& handler) {
    QAction* act = menu->addAction(text);
    act->setShortcuts(shortcuts);
    act->setShortcutContext(Qt::WindowShortcut);
    connect(act, &QAction::triggered, this, [handler](bool) { handler(); });
    // 同时挂到主窗口：全屏隐藏菜单栏后快捷键依然有效
    addAction(act);
    return act;
}

void MainWindow::setupMenuBar() {
    // ── 文件 ──
    QMenu* fileMenu = menuBar()->addMenu(tr("文件(&F)"));
    addShortcutAction(fileMenu, tr("打开文件..."), {QKeySequence::Open},
                      [this] { openFile(); });
    addShortcutAction(fileMenu, tr("打开 URL..."), {QKeySequence(Qt::CTRL | Qt::Key_U)},
                      [this] { openUrl(); });
    fileMenu->addSeparator();
    addShortcutAction(fileMenu, tr("退出"), {QKeySequence::Quit},
                      [] { qApp->closeAllWindows(); });

    // ── 播放 ──
    QMenu* playMenu = menuBar()->addMenu(tr("播放(&P)"));
    addShortcutAction(playMenu, tr("播放/暂停"), {QKeySequence(Qt::Key_Space)},
                      [this] { onPlayPause(); });
    addShortcutAction(playMenu, tr("停止"), {QKeySequence(Qt::Key_S)},
                      [this] { onStop(); });
    playMenu->addSeparator();
    addShortcutAction(playMenu, tr("快退 %1 秒").arg(kSeekStepShort), {QKeySequence(Qt::Key_Left)},
                      [this] { controller_->seekRelative(-kSeekStepShort); });
    addShortcutAction(playMenu, tr("快进 %1 秒").arg(kSeekStepShort), {QKeySequence(Qt::Key_Right)},
                      [this] { controller_->seekRelative(kSeekStepShort); });
    addShortcutAction(playMenu, tr("快退 %1 秒").arg(kSeekStepLong), {QKeySequence(Qt::CTRL | Qt::Key_Left)},
                      [this] { controller_->seekRelative(-kSeekStepLong); });
    addShortcutAction(playMenu, tr("快进 %1 秒").arg(kSeekStepLong), {QKeySequence(Qt::CTRL | Qt::Key_Right)},
                      [this] { controller_->seekRelative(kSeekStepLong); });
    playMenu->addSeparator();
    addShortcutAction(playMenu, tr("上一帧"), {QKeySequence(Qt::Key_Comma)},
                      [this] { controller_->stepBackward(); });
    addShortcutAction(playMenu, tr("下一帧"), {QKeySequence(Qt::Key_Period)},
                      [this] { controller_->stepForward(); });
    playMenu->addSeparator();

    loopAction_ = addShortcutAction(playMenu, tr("循环播放"), {QKeySequence(Qt::Key_L)},
                                    [this] { controller_->setLoop(loopAction_->isChecked()); });
    loopAction_->setCheckable(true);
    loopAction_->setChecked(false);

    QMenu* speedMenu = playMenu->addMenu(tr("倍速"));
    speedGroup_ = new QActionGroup(this);
    speedGroup_->setExclusive(true);
    for (double s : speedPresets_) {
        QAction* a = speedMenu->addAction(QStringLiteral("%1x").arg(s, 0, 'g', 3));
        a->setCheckable(true);
        a->setData(s);
        a->setChecked(std::fabs(s - 1.0) < 1e-9);
        speedGroup_->addAction(a);
        connect(a, &QAction::triggered, this, [this, s] { controller_->setSpeed(s); });
    }
    speedMenu->addSeparator();
    addShortcutAction(speedMenu, tr("减速"), {QKeySequence(Qt::Key_BracketLeft)},
                      [this] { onSpeedStep(-1); });
    addShortcutAction(speedMenu, tr("加速"), {QKeySequence(Qt::Key_BracketRight)},
                      [this] { onSpeedStep(+1); });
    addShortcutAction(speedMenu, tr("正常速度"), {QKeySequence(Qt::Key_Backspace)},
                      [this] { controller_->setSpeed(1.0); });
    playMenu->addSeparator();

    QAction* hwAction = playMenu->addAction(tr("硬件解码 (DXVA2)"));
    hwAction->setCheckable(true);
    hwAction->setChecked(false);
    connect(hwAction, &QAction::toggled, this, [this](bool enabled) {
        controller_->setHardwareDecode(enabled);
        if (controller_->state() != PlayState::Idle && controller_->state() != PlayState::Stopped) {
            statusBar()->showMessage(tr("硬件解码切换将在下次打开文件时生效"), 4000);
        }
    });

    // ── 音频 ──
    QMenu* audioMenu = menuBar()->addMenu(tr("音频(&A)"));
    muteAction_ = addShortcutAction(audioMenu, tr("静音"), {QKeySequence(Qt::Key_M)},
                                    [this] { controls_->setMuted(muteAction_->isChecked()); });
    muteAction_->setCheckable(true);
    addShortcutAction(audioMenu, tr("音量 +"), {QKeySequence(Qt::Key_Up)},
                      [this] { controls_->setVolume(controls_->volume() + kVolumeStep); });
    addShortcutAction(audioMenu, tr("音量 -"), {QKeySequence(Qt::Key_Down)},
                      [this] { controls_->setVolume(controls_->volume() - kVolumeStep); });

    // ── 视图 ──
    QMenu* viewMenu = menuBar()->addMenu(tr("视图(&V)"));
    fullScreenAction_ = addShortcutAction(viewMenu, tr("全屏"),
                                          {QKeySequence(Qt::Key_F), QKeySequence(Qt::Key_F11)},
                                          [this] { setFullScreen(fullScreenAction_->isChecked()); });
    fullScreenAction_->setCheckable(true);
    QAction* escAction = new QAction(tr("退出全屏"), this);
    escAction->setShortcut(QKeySequence(Qt::Key_Escape));
    escAction->setShortcutContext(Qt::WindowShortcut);
    connect(escAction, &QAction::triggered, this, [this] { if (fullScreen_) setFullScreen(false); });
    addAction(escAction);
    viewMenu->addSeparator();

    eqAction_ = viewMenu->addAction(tr("均衡器面板"));
    eqAction_->setCheckable(true);
    eqAction_->setChecked(true);
    connect(eqAction_, &QAction::toggled, this, [this](bool visible) {
        if (eqPanel_ && !fullScreen_) eqPanel_->setVisible(visible);
    });

    specAction_ = viewMenu->addAction(tr("频谱显示"));
    specAction_->setCheckable(true);
    specAction_->setChecked(true);
    connect(specAction_, &QAction::toggled, this, [this](bool visible) {
        if (spectrumWidget_ && !fullScreen_) spectrumWidget_->setVisible(visible);
    });
}

void MainWindow::setupUI() {
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    videoWidget_ = new VideoWidget(central);

    spectrumWidget_ = new SpectrumWidget(central);
    spectrumWidget_->setAnalyzer(controller_->spectrumAnalyzer());

    eqPanel_ = new EqualizerPanel(controller_->equalizer(), central);

    controls_ = new PlaybackControls(central);

    layout->addWidget(videoWidget_, 1);
    layout->addWidget(spectrumWidget_, 0);
    layout->addWidget(eqPanel_, 0);
    layout->addWidget(controls_, 0);

    setCentralWidget(central);

    connect(controls_, &PlaybackControls::playPauseClicked,  this, &MainWindow::onPlayPause);
    connect(controls_, &PlaybackControls::stopClicked,       this, &MainWindow::onStop);
    connect(controls_, &PlaybackControls::seekRequested,     this, &MainWindow::onSeek);
    connect(controls_, &PlaybackControls::speedCycleRequested, this, &MainWindow::onSpeedCycle);
    connect(controls_, &PlaybackControls::stepForwardClicked,
            controller_, &PlayerController::stepForward);
    connect(controls_, &PlaybackControls::stepBackwardClicked,
            controller_, &PlayerController::stepBackward);
    connect(controls_, &PlaybackControls::volumeChanged,     this, &MainWindow::onVolumeChanged);
    connect(controls_, &PlaybackControls::muteToggled,       this, &MainWindow::onMuteToggled);
    connect(controls_, &PlaybackControls::loopToggled,       this, &MainWindow::onLoopToggled);

    connect(videoWidget_, &VideoWidget::contentResolutionChanged,
            controller_, &PlayerController::setVideoTargetSize);
    connect(videoWidget_, &VideoWidget::doubleClicked,
            this, &MainWindow::toggleFullScreen);
}
