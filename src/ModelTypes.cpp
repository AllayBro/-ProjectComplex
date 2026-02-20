#pragma once
#include <QMainWindow>
#include "AppConfig.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const AppConfig& cfg, const QString& appDirPath, QWidget* parent = nullptr);

private:
    AppConfig m_cfg;
    QString m_appDir;
};