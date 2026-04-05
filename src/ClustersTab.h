#pragma once

#include <QWidget>
#include <QString>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QVector>
#include <QImage>
#include <QJsonObject>
#include "AppConfig.h"
#include "ModelTypes.h"

class RunnerClient;
class ResultView;
class ClustersTab : public QWidget {
    Q_OBJECT
public:
    explicit ClustersTab(const AppConfig& cfg, const QString& appDir, QWidget* parent = nullptr);
    void setYoloModelPath(const QString& absPath);
    void setDeviceMode(const QString& mode);
    QString currentImagePath() const;
    QString currentDeviceMode() const;
    signals:
        void imageSelected(const QString& imagePath);
    void resultReady(const QString& imagePath, const ModuleResult& result);
    void yoloModelChanged(const QString& absPath);
    void deviceModeChanged(const QString& mode);
private:
    AppConfig m_cfg;
    QString m_appDir;

    RunnerClient* m_runner = nullptr;

    QLineEdit* m_input = nullptr;
    QPushButton* m_browse = nullptr;

    QComboBox* m_yoloModel = nullptr;
    QPushButton* m_browseYolo = nullptr;

    QComboBox* m_device = nullptr;

    QVector<QPushButton*> m_clusterButtons;

    ResultView* m_view = nullptr;

    QString m_lastRunImagePath;

    QString yoloDirAbs() const;
    void reloadYoloModels();
    QString currentYoloModelPath() const;
    void rememberYoloModelPath(const QString& absPath);

    void applyPreview(const QString& imagePath);
    bool runPreviewTaskRaw(const QString& inputPath, QImage& outImage, QJsonObject& outExifRoot, QString& err);

    void bindRunner();
};