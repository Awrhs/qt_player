#include "MainWindow.h"
#include "VideoWidget.h"
#include "PlaybackControls.h"
#include "SpectrumWidget.h"
#include "EqualizerPanel.h"
#include "../core/PlayerController.h"

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QSplitter>
#include <QStatusBar>
#include <QApplication>
#include <QFileInfo>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Qt Player"));
    resize(960, 720);

    controller_ = new PlayerController(this);

    setupUI();
    setupMenuBar();

    // 位置更新定时器
    positionTimer_ = new QTimer(this);
    connect(positionTimer_, &QTimer::timeout, this, &MainWindow::onPositionUpdate);

    // PlayerController 信号
    connect(controller_, &PlayerController::frameReady,
            this, &MainWindow::onFrameReady);
    connect(controller_, &PlayerController::stateChanged,
            this, &MainWindow::onStateChanged);
    connect(controller_, &PlayerController::errorOccurred,
            this, &MainWindow::onError);
    connect(controller_, &PlayerController::durationChanged,
            this, [this](double) {});
}

MainWindow::~MainWindow() = default;

void MainWindow::openFile() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("打开媒体文件"), QString(),
        tr("媒体文件 (*.mp4 *.ts *.flv *.avi *.mkv *.mov *.wmv *.mp3 *.aac *.m4a);;所有文件 (*)"));
    if (!path.isEmpty()) {
        openPath(path);
    }
}

void MainWindow::openPath(const QString& path) {
    statusBar()->showMessage(tr("正在打开: %1...").arg(QFileInfo(path).fileName()));
    controller_->open(path);
}

void MainWindow::onPlayPause() {
    if (controller_->state() == PlayState::Playing) {
        controller_->pause();
    } else if (controller_->state() == PlayState::Paused) {
        controller_->play();
    }
}

void MainWindow::onSeek(double position) {
    controller_->seek(position);
}

void MainWindow::onSpeedChanged(double factor) {
    controller_->setSpeed(factor);
}

void MainWindow::onVolumeChanged(int percent) {
    Q_UNUSED(percent)
}

void MainWindow::onMuteToggled(bool muted) {
    Q_UNUSED(muted)
}

void MainWindow::onStateChanged(PlayState state) {
    switch (state) {
    case PlayState::Playing:
        controls_->setPlaying(true);
        controls_->setStepEnabled(false);
        positionTimer_->start(250);
        spectrumWidget_->start(true);
        statusBar()->showMessage(tr("播放中"));
        break;
    case PlayState::Paused:
        controls_->setPlaying(false);
        controls_->setStepEnabled(true);
        positionTimer_->stop();
        spectrumWidget_->start(false);
        statusBar()->showMessage(tr("已暂停"));
        break;
    case PlayState::Stopped:
        controls_->setPlaying(false);
        controls_->setStepEnabled(false);
        positionTimer_->stop();
        spectrumWidget_->start(false);
        videoWidget_->clearFrame();
        statusBar()->showMessage(tr("已停止"));
        break;
    case PlayState::Idle:
        videoWidget_->clearFrame();
        controls_->setStepEnabled(false);
        statusBar()->showMessage(tr("就绪"));
        break;
    default:
        break;
    }
}

void MainWindow::onFrameReady(const QImage& image) {
    videoWidget_->setFrame(image);
}

void MainWindow::onError(const QString& msg) {
    statusBar()->showMessage(tr("错误: %1").arg(msg));
    QMessageBox::warning(this, tr("播放器错误"), msg);
}

void MainWindow::onPositionUpdate() {
    if (controller_) {
        controls_->updatePosition(controller_->position(), controller_->duration());
    }
}

void MainWindow::setupMenuBar() {
    QMenu* fileMenu = menuBar()->addMenu(tr("文件"));
    QAction* openAction = fileMenu->addAction(tr("打开..."), this, &MainWindow::openFile);
    openAction->setShortcut(QKeySequence::Open);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("退出"), qApp, &QApplication::quit, QKeySequence::Quit);

    QMenu* playMenu = menuBar()->addMenu(tr("播放"));
    playMenu->addAction(tr("播放/暂停"), this, &MainWindow::onPlayPause, QKeySequence(Qt::Key_Space));
    playMenu->addAction(tr("停止"), controller_, &PlayerController::stop, QKeySequence(Qt::Key_Escape));
    playMenu->addAction(tr("逐帧前进"), controller_, &PlayerController::stepForward, QKeySequence(Qt::Key_Right));
    playMenu->addAction(tr("逐帧后退"), controller_, &PlayerController::stepBackward, QKeySequence(Qt::Key_Left));
    playMenu->addSeparator();

    QAction* hwAction = playMenu->addAction(tr("硬件解码 (DXVA2)"));
    hwAction->setCheckable(true);
    hwAction->setChecked(false);
    connect(hwAction, &QAction::toggled, this, [this](bool enabled) {
        controller_->setHardwareDecode(enabled);
        if (controller_->state() != PlayState::Idle && controller_->state() != PlayState::Stopped) {
            QMessageBox::information(this, tr("提示"),
                tr("硬件解码切换将在下次打开文件时生效"));
        }
    });

    QMenu* viewMenu = menuBar()->addMenu(tr("视图"));
    QAction* eqAction = viewMenu->addAction(tr("均衡器面板"));
    eqAction->setCheckable(true);
    eqAction->setChecked(true);
    connect(eqAction, &QAction::toggled, this, [this](bool visible) {
        if (eqPanel_) eqPanel_->setVisible(visible);
    });

    QAction* specAction = viewMenu->addAction(tr("频谱显示"));
    specAction->setCheckable(true);
    specAction->setChecked(true);
    connect(specAction, &QAction::toggled, this, [this](bool visible) {
        if (spectrumWidget_) spectrumWidget_->setVisible(visible);
    });
}

void MainWindow::setupUI() {
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    videoWidget_ = new VideoWidget(central);

    // 频谱（在视频下方）
    spectrumWidget_ = new SpectrumWidget(central);
    spectrumWidget_->setAnalyzer(controller_->spectrumAnalyzer());

    // 均衡器面板（在频谱下方）
    eqPanel_ = new EqualizerPanel(controller_->equalizer(), central);

    controls_ = new PlaybackControls(central);

    layout->addWidget(videoWidget_, 1);
    layout->addWidget(spectrumWidget_, 0);
    layout->addWidget(eqPanel_, 0);
    layout->addWidget(controls_, 0);

    setCentralWidget(central);

    // 控制栏信号
    connect(controls_, &PlaybackControls::playPauseClicked,
            this, &MainWindow::onPlayPause);
    connect(controls_, &PlaybackControls::seekRequested,
            this, &MainWindow::onSeek);
    connect(controls_, &PlaybackControls::speedChanged,
            this, &MainWindow::onSpeedChanged);
    connect(controls_, &PlaybackControls::stepForwardClicked,
            controller_, &PlayerController::stepForward);
    connect(controls_, &PlaybackControls::stepBackwardClicked,
            controller_, &PlayerController::stepBackward);
    connect(controls_, &PlaybackControls::volumeChanged,
            this, &MainWindow::onVolumeChanged);
    connect(controls_, &PlaybackControls::muteToggled,
            this, &MainWindow::onMuteToggled);
}