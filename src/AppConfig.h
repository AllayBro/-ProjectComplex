#pragma once

#include <QString>
#include <QVector>
#include <QJsonObject>

struct ClusterButtonSpec {
    int clusterId = 0;
    QString title;
};

struct MapConfig {
    bool startOffline = false;
    QString offlineTilesDir;
    QString cacheDir;
    QString userAgent = "vk_qt_app/1.0";

    int probeIntervalMs = 0;
    QString probeUrl;
    int probeTimeoutMs = 0;
};

class AppConfig {
public:
    QString pythonExe = "python";
    QString runnerScript = "python/runner.py";
    QString pythonConfigJson = "python/default_config.json";

    QString yoloDir = "yolo";
    QString yoloModelPath = "yolo11n.pt";

    bool uiShowRegressionTab = false;

    QVector<ClusterButtonSpec> clusters;
    MapConfig map;

    static AppConfig loadOrDie(const QString& appDirPath);

    QJsonObject toRunConfigPatch(const QString& deviceMode) const;

private:
    static QString readAllText(const QString& path);
    static QJsonObject readJsonObjectOrDie(const QString& path);
};