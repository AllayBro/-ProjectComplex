#include "MainWindow.h"

#include <QTabWidget>

#include "RegressionTab.h"
#include "ClustersTab.h"
#include "FullDistanceTab.h"

MainWindow::MainWindow(const AppConfig& cfg, const QString& appDirPath, QWidget* parent)
    : QMainWindow(parent), m_cfg(cfg), m_appDir(appDirPath) {

    auto* tabs = new QTabWidget(this);

    tabs->addTab(new RegressionTab(this), "Регрессия");
    tabs->addTab(new ClustersTab(m_cfg, m_appDir, this), "Кластеры (4)");
    tabs->addTab(new FullDistanceTab(m_cfg, m_appDir, this), "Полный режим расстояния");

    setCentralWidget(tabs);
    setWindowTitle("ВКР: Кластеры + Расстояние");
    resize(1200, 820);
}