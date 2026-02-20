#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QVector>

#include "RunnerClient.h"
#include "ResultView.h"
#include "AppConfig.h"

class ClustersTab : public QWidget {
    Q_OBJECT
public:
    explicit ClustersTab(const AppConfig& cfg, const QString& appDir, QWidget* parent = nullptr);

private:
    AppConfig m_cfg;
    RunnerClient* m_runner = nullptr;

    QLineEdit* m_input = nullptr;
    QPushButton* m_browse = nullptr;

    QLineEdit* m_outputDir = nullptr;
    QPushButton* m_browseOut = nullptr;

    QComboBox* m_device = nullptr;

    QVector<QPushButton*> m_clusterButtons;
    ResultView* m_view = nullptr;

    void bindRunner();
};