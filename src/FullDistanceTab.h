#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>

#include "RunnerClient.h"
#include "ResultView.h"
#include "AppConfig.h"

class FullDistanceTab : public QWidget {
    Q_OBJECT
public:
    explicit FullDistanceTab(const AppConfig& cfg, const QString& appDir, QWidget* parent = nullptr);

private:
    RunnerClient* m_runner = nullptr;

    QLineEdit* m_input = nullptr;
    QPushButton* m_browse = nullptr;

    QLineEdit* m_outputDir = nullptr;
    QPushButton* m_browseOut = nullptr;

    QComboBox* m_device = nullptr;

    QPushButton* m_run = nullptr;
    ResultView* m_view = nullptr;

    void bindRunner();
};