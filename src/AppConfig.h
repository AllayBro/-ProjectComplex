#pragma once
#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>

struct ClusterButtonSpec {
    int clusterId = 0;
    QString title;
};

class AppConfig {
public:
    QString pythonExe;
    QString runnerScript;
    QString pythonConfigJson;

    QVector<ClusterButtonSpec> clusters;

    static AppConfig loadOrDie(const QString& appDirPath);
    QJsonObject toRunConfigPatch(const QString& deviceMode) const;

private:
    static QString readAllText(const QString& path);
    static QJsonObject readJsonObjectOrDie(const QString& path);
};
