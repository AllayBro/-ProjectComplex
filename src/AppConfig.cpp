#include "AppConfig.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QCoreApplication>

QString AppConfig::readAllText(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QJsonObject AppConfig::readJsonObjectOrDie(const QString& path) {
    QString txt = readAllText(path);
    if (txt.isEmpty()) qFatal("Cannot read config file: %s", qPrintable(path));
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(txt.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError) qFatal("JSON parse error in %s: %s", qPrintable(path), qPrintable(pe.errorString()));
    if (!doc.isObject()) qFatal("Config is not a JSON object: %s", qPrintable(path));
    return doc.object();
}

AppConfig AppConfig::loadOrDie(const QString& appDirPath) {
    AppConfig cfg;
    QString path = QDir(appDirPath).filePath("config/app_config.json");
    QJsonObject o = readJsonObjectOrDie(path);

    cfg.pythonExe = o.value("python_exe").toString("python");
    cfg.runnerScript = o.value("runner_script").toString("python/runner.py");
    cfg.pythonConfigJson = o.value("python_config_json").toString("python/default_config.json");

    cfg.clusters.clear();
    QJsonArray ca = o.value("clusters").toArray();
    if (ca.size() != 4) qFatal("Config must contain exactly 4 clusters in config/app_config.json");
    for (const auto& v : ca) {
        QJsonObject co = v.toObject();
        ClusterButtonSpec s;
        s.clusterId = co.value("cluster_id").toInt();
        s.title = co.value("title").toString();
        if (s.clusterId <= 0 || s.title.isEmpty()) qFatal("Invalid cluster spec in config/app_config.json");
        cfg.clusters.push_back(s);
    }
    return cfg;
}

QJsonObject AppConfig::toRunConfigPatch(const QString& deviceMode) const {
    QJsonObject patch;
    QJsonObject dev;
    dev.insert("device_mode", deviceMode);
    dev.insert("fallback_to_cpu", true);
    patch.insert("device", dev);
    return patch;
}
