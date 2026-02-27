#pragma once
#include <QMainWindow>
#include <QString>
#include <functional>

#include "AppConfig.h"

class QTabWidget;
class RegressionTab;
class ClustersTab;
class FullDistanceTab;
class MapTab;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    using ProgressCb = std::function<void(int, const QString&)>;

    explicit MainWindow(const AppConfig& cfg, const QString& appDirPath, QWidget* parent = nullptr);
    explicit MainWindow(const AppConfig& cfg, const QString& appDirPath, ProgressCb progressCb, QWidget* parent = nullptr);

private:
    AppConfig m_cfg;
    QString m_appDir;
    ProgressCb m_progress;

    QTabWidget* m_tabs = nullptr;

    RegressionTab* m_reg = nullptr;
    MapTab* m_map = nullptr;
    ClustersTab* m_clusters = nullptr;
    FullDistanceTab* m_full = nullptr;

    void setupUi();
    void bindSignals();
    void step(int percent, const QString& msg);
};