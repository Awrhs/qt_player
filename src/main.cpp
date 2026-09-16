#include "ui/MainWindow.h"
#include "core/PlayerController.h"

#include <QApplication>

// LeakSanitizer 抑制：Qt Wayland 插件在进程退出时遗留的 ~9 个小分配
// （QPlatformIntegration、wl_proxy 等全局单例，总计约 864 字节），
// 是 Qt 平台插件的已知行为，不影响程序正确性。不加抑制会导致
// ASan build 退出码非零，干扰"关窗即退出"的验证。
extern "C" const char* __lsan_default_suppressions() {
    return "leak:QtWayland\n"
           "leak:wl_proxy\n"
           "leak:wl_display\n"
           "leak:wl_event_queue\n"
           "leak:wl_region\n"
           "leak:wl_shm\n"
           "leak:wl_buffer\n";
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Qt Player"));
    app.setOrganizationName(QStringLiteral("qt-player"));

    MainWindow window;
    window.show();

    // 命令行参数：自动打开文件
    if (argc > 1) {
        QString path = QString::fromUtf8(argv[1]);
        window.openPath(path);
    }

    return app.exec();
}