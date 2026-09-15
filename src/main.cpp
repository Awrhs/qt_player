#include "ui/MainWindow.h"
#include "core/PlayerController.h"

#include <QApplication>

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