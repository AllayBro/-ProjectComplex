#include "MainWindow.h"

#include <QTabWidget>

#include "RegressionTab.h"
#include "ClustersTab.h"
#include "FullDistanceTab.h"
#include "MapTab.h"

MainWindow::MainWindow(const AppConfig& cfg, const QString& appDirPath, QWidget* parent)
    : MainWindow(cfg, appDirPath, ProgressCb{}, parent) {}

MainWindow::MainWindow(const AppConfig& cfg, const QString& appDirPath, ProgressCb progressCb, QWidget* parent)
    : QMainWindow(parent), m_cfg(cfg), m_appDir(appDirPath), m_progress(std::move(progressCb)) {

    setupUi();
    bindSignals();

    setCentralWidget(m_tabs);
    setWindowTitle("ВКР: Кластеры + Расстояние");
    resize(1200, 820);
}

void MainWindow::step(int percent, const QString& msg) {
    if (m_progress) m_progress(percent, msg);
}

void MainWindow::setupUi() {
    step(55, "Интерфейс: вкладки...");

    m_tabs = new QTabWidget(this);

    if (m_cfg.uiShowRegressionTab) {
        m_reg = new RegressionTab(this);
        step(62, "Интерфейс: регрессия...");
    }

    m_map = new MapTab(m_cfg, this);
    step(70, "Интерфейс: карта...");

    m_clusters = new ClustersTab(m_cfg, m_appDir, this);
    step(80, "Интерфейс: кластеры...");

    m_full = new FullDistanceTab(m_cfg, m_appDir, this);
    step(90, "Интерфейс: полный режим...");

    if (m_reg) m_tabs->addTab(m_reg, "Регрессия");
    m_tabs->addTab(m_map, "Карта");
    m_tabs->addTab(m_clusters, "Режим кластеров");
    m_tabs->addTab(m_full, "Полный режим расстояния");

    step(96, "Интерфейс: готово");
}

void MainWindow::bindSignals() {
    connect(m_clusters, &ClustersTab::imageSelected, m_map, &MapTab::onImageSelected);
    connect(m_clusters, &ClustersTab::resultReady,   m_map, &MapTab::onResultReady);

    connect(m_full, &FullDistanceTab::imageSelected, m_map, &MapTab::onImageSelected);
    connect(m_full, &FullDistanceTab::resultReady,   m_map, &MapTab::onResultReady);

    connect(m_clusters, &ClustersTab::yoloModelChanged, m_full, &FullDistanceTab::setYoloModelPath);
    connect(m_full, &FullDistanceTab::yoloModelChanged, m_clusters, &ClustersTab::setYoloModelPath);

    connect(m_clusters, &ClustersTab::deviceModeChanged, m_full, &FullDistanceTab::setDeviceMode);
    connect(m_full, &FullDistanceTab::deviceModeChanged, m_clusters, &ClustersTab::setDeviceMode);
    connect(m_clusters, &ClustersTab::deviceModeChanged, m_map, &MapTab::setDeviceMode);
    connect(m_full, &FullDistanceTab::deviceModeChanged, m_map, &MapTab::setDeviceMode);


    if (m_clusters) {
        const QString p = m_clusters->currentImagePath();
        if (!p.isEmpty()) {
            m_map->onImageSelected(p);
        }
    }

    if (m_full) {
        const QString p = m_full->currentImagePath();
        if (!p.isEmpty()) {
            m_map->onImageSelected(p);
        }
    }
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        QString p;

        QWidget* current = m_tabs->widget(index);

        if (current == m_full && m_full) {
            p = m_full->currentImagePath();
        } else if (current == m_clusters && m_clusters) {
            p = m_clusters->currentImagePath();
        } else {
            if (m_full) p = m_full->currentImagePath();
            if (p.isEmpty() && m_clusters) p = m_clusters->currentImagePath();
        }

        if (!p.isEmpty()) {
            m_map->onImageSelected(p);
        }
    });
}