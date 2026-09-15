# Qt Player - FFmpeg/Qt6 桌面媒体播放器

基于 FFmpeg 和 Qt6 的桌面媒体播放器，完整实现「解复用 → 解码 → 同步 → 渲染」播放管线，集成 DXVA2 硬件解码、音频频谱可视化和 10 段均衡器。

## 功能特性

### 播放控制
- ✓ 本地文件播放：MP4 / TS / FLV / AVI 容器，H.264 / H.265 / AAC / MP3 编码
- ✓ 播放/暂停/停止
- ✓ 进度条拖动 seek（响应 ≤ 200ms）
- ✓ 倍速播放 0.5x ~ 2.0x
- ✓ 逐帧前进/后退（暂停态可用）
- ✓ 音量调节 0~100% + 静音

### 音视频同步
- ✓ 音频主时钟同步（误差 ≤ 80ms）
- ✓ 无音频流时外部时钟兜底
- ✓ 视频帧提前等待 / 落后丢帧

### 硬件解码
- ✓ DXVA2 硬件解码（回读模式）
- ✓ 硬解初始化失败自动回退软解
- ✓ 强制软解开关

### 音频增强
- ✓ 24 段频谱柱状图（自实现 2048 点基-2 FFT）
- ✓ 10 段 ISO 均衡器（RBJ 峰值滤波器，±12dB）
- ✓ 5 种预设：Flat / Pop / Rock / Classical / Jazz

### 网络播放
- ✓ HTTP(S) 点播流
- ✓ 滑动窗口缓存（可配，默认 4MB）
- ✓ 缓冲等待 + 超时保护

### 其他
- ✓ 断点续播
- ✓ 多线程架构（解复用/音频解码/视频解码分离）
- ✓ 解复用背压控制
- ✓ 错误安全：坏文件不崩溃

## 构建步骤

### 环境要求

| 组件 | 版本 |
|---|---|
| CMake | ≥ 3.16 |
| C++ 编译器 | GCC ≥ 11 / MSVC 2022 / Clang ≥ 14 |
| Qt | ≥ 6.5（Widgets + Multimedia） |
| FFmpeg | ≥ 5.0（libavformat/libavcodec/libavutil/libswresample/libswscale） |

### Windows (MSYS2 MINGW64)

```bash
# 安装依赖
pacman -S mingw-w64-x86_64-toolchain
pacman -S mingw-w64-x86_64-qt6-base
pacman -S mingw-w64-x86_64-ffmpeg
pacman -S mingw-w64-x86_64-cmake

# 构建
cd qt_player
cmake -B build
cmake --build build

# 运行
./build/qt_player.exe <media_file>
```

### Windows (Visual Studio)

```powershell
# 安装 Qt6 for MSVC + FFmpeg dev SDK
# 设置环境变量
set CMAKE_PREFIX_PATH=C:\Qt\6.7.0\msvc2022_64
set FFMPEG_DIR=C:\path\to\ffmpeg-dev

# 从 Developer Command Prompt 构建
cmake -B build -G "Visual Studio 17 2022"
cmake --build build
```

### Linux

```bash
sudo apt install qt6-base-dev libqt6multimedia6 qt6-multimedia-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev

cmake -B build
cmake --build build
```

> **注意**：Linux 不支持 DXVA2（Windows 独占），硬解开关将不可用。

## 基准测试

> 测试环境：待填充（欢迎提交 PR）

| 指标 | 结果 | 目标 |
|---|---|---|
| 本地 1080p 起播时间 | - | ≤ 1.0s |
| 软解 CPU 占用（1080p H.264） | - | ≤ 40% |
| 硬解 CPU 占用（同片源） | - | ≤ 15% |
| 音画同步误差 | - | ≤ 80ms |
| seek 响应（p95） | - | ≤ 200ms |
| 内存峰值（1080p） | - | ≤ 300MB |
| 频谱刷新率 | - | 30fps |

## 架构概述

### 线程模型

```
┌─ 主线程（Qt UI）───────────────┐
│ MainWindow / 渲染定时器 (15ms)    │
│ SpectrumWidget (33ms)           │
└──────────┬──────────────────────┘
           │ 命令队列
┌──────────▼──────────────────────┐
│ DemuxThread                     │
│   av_read_frame → 音频包/视频包  │
├─────────────────────────────────┤
│ AudioDecodeThread               │
│   解码 → swr_convert → EQ → PCM │
├─────────────────────────────────┤
│ VideoDecodeThread               │
│   解码（软/硬）→ 帧队列           │
└─────────────────────────────────┘
```

### 数据流

```
文件/URL → AVIOContext → av_read_frame → AVPacket → PacketQueue
→ AVCodecContext → AVFrame → FrameQueue
音频 → swr_convert → Equalizer(10 biquad) → PCM RingBuffer → QAudioSink
                      └→ SpectrumAnalyzer(FFT 2048 → 24 bands)
视频 → AudioClock sync → sws_scale(YUV→RGBA) → QImage → paintEvent
```

### 关键类

| 类 | 职责 |
|---|---|
| PlayerController | 状态机 + 线程管理 + 命令队列 |
| Demuxer | AVFormatContext 封装 |
| Decoder | 软/硬解码器封装 |
| VideoRenderer | sws_scale → QImage |
| AudioOutput | 重采样 + QAudioSink 输出 |
| AudioClock | 基于样本消耗的音频时钟 |
| Equalizer | 10 段 RBJ IIR 级联 |
| SpectrumAnalyzer | 自实现 FFT + 24 段对数分档 |
| BoundedQueue | 线程安全有界阻塞队列 |
| RingBuffer | 线程安全环形缓冲 |
| ResumeManager | QSettings 断点持久化 |

## 项目结构

```
qt_player/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── core/          # 播放内核
│   │   ├── BoundedQueue.h
│   │   ├── Demuxer.h/cpp
│   │   ├── Decoder.h/cpp
│   │   ├── VideoRenderer.h/cpp
│   │   ├── AudioClock.h/cpp
│   │   ├── AudioOutput.h/cpp
│   │   ├── Equalizer.h/cpp
│   │   ├── SpectrumAnalyzer.h/cpp
│   │   └── PlayerController.h/cpp
│   ├── network/        # 网络流
│   │   ├── RingBuffer.h/cpp
│   │   ├── NetPrefetcher.h/cpp
│   │   └── CustomAVIO.h/cpp
│   ├── ui/             # 用户界面
│   │   ├── MainWindow.h/cpp
│   │   ├── VideoWidget.h/cpp
│   │   ├── SpectrumWidget.h/cpp
│   │   ├── EqualizerPanel.h/cpp
│   │   └── PlaybackControls.h/cpp
│   └── utils/          # 工具
│       ├── Log.h/cpp
│       └── ResumeManager.h/cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── test_bounded_queue.cpp
│   ├── test_fft.cpp
│   ├── test_equalizer.cpp
│   └── test_audio_clock.cpp
└── README.md
```

## 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| BoundedQueue | 自实现 | 面试可讲；免引入依赖 |
| FFT | 自实现基-2 Cooley-Tukey | 学习目标 |
| 渲染方式 | QWidget + QImage | 实现简单 |
| 同步策略 | 音频主时钟 | 人耳最敏感；ffplay 标准 |
| 硬解路线 | DXVA2 回读模式 | 编码简单；v2 可升级 D3D9 互操作 |

## 许可

本项目基于 GPLv3 许可发布。

FFmpeg 是 LGPG/GPL 许可的第三方库。Qt 是 LGPL 许可的第三方库。

## 简历声明对应

| 简历能力点 | 项目模块 | 对应 PRD |
|---|---|---|
| 基于 libavformat 解复用 | Demuxer | FR-01 |
| 基于 libavcodec 解码 | Decoder | FR-01 |
| 播放/暂停/seek/倍速/逐帧 | PlayerController | FR-02 |
| PTS 同步、time_base 转换 | AudioClock + VideoSync | FR-03 |
| DXVA2 硬解 + 回退 | Decoder (HW path) | FR-04 |
| FFT 频谱 | SpectrumAnalyzer | FR-05 |
| 10 段均衡器 | Equalizer | FR-06 |
| 多线程分离架构 | 线程模型 | §5.2 |
| 滑动窗口缓存 / 断点续播 | RingBuffer / ResumeManager | FR-07 |