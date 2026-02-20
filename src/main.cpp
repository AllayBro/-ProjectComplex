#include <QApplication>
#include <QCoreApplication>

#include "MainWindow.h"
#include "AppConfig.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    const QString appDir = QCoreApplication::applicationDirPath();
    AppConfig cfg = AppConfig::loadOrDie(appDir);

    MainWindow w(cfg, appDir);
    w.show();

    return app.exec();
}