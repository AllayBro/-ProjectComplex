#include "AppConfig.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QStandardPaths>

QString AppConfig::readAllText(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QJsonObject AppConfig::readJsonObjectOrDie(const QString& path) {
    const QString txt = readAllText(path);
    if (txt.isEmpty()) qFatal("Cannot read config file: %s", qPrintable(path));

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(txt.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError)
        qFatal("JSON parse error in %s: %s", qPrintable(path), qPrintable(pe.errorString()));
    if (!doc.isObject()) qFatal("Config is not a JSON object: %s", qPrintable(path));

    return doc.object();
}

static QString defaultMapCacheDir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) base = QDir::homePath();
    QDir d(base);
    d.mkpath(".");
    return d.filePath("map_cache");
}

AppConfig AppConfig::loadOrDie(const QString& appDirPath) {
    AppConfig cfg;

    const QString path = QDir(appDirPath).filePath("config/app_config.json");
    const QJsonObject o = readJsonObjectOrDie(path);

    cfg.pythonExe = o.value("python_exe").toString("python").trimmed();
    if (cfg.pythonExe.isEmpty()) cfg.pythonExe = "python";

    cfg.runnerScript = o.value("runner_script").toString("python/runner.py").trimmed();
    if (cfg.runnerScript.isEmpty()) cfg.runnerScript = "python/runner.py";

    cfg.pythonConfigJson = o.value("python_config_json").toString("python/default_config.json").trimmed();
    if (cfg.pythonConfigJson.isEmpty()) cfg.pythonConfigJson = "python/default_config.json";

    cfg.yoloDir = o.value("yolo_dir").toString("yolo").trimmed();
    if (cfg.yoloDir.isEmpty()) cfg.yoloDir = "yolo";

    cfg.yoloModelPath = o.value("yolo_model_path").toString("yolo11n.pt").trimmed();
    if (cfg.yoloModelPath.isEmpty()) cfg.yoloModelPath = "yolo11n.pt";

    cfg.clusters.clear();
    const QJsonArray ca = o.value("clusters").toArray();
    if (ca.size() != 4) qFatal("Config must contain exactly 4 clusters in config/app_config.json");

    for (const auto& v : ca) {
        const QJsonObject co = v.toObject();
        ClusterButtonSpec s;
        s.clusterId = co.value("cluster_id").toInt();
        s.title = co.value("title").toString().trimmed();
        if (s.clusterId <= 0 || s.title.isEmpty()) qFatal("Invalid cluster spec in config/app_config.json");
        cfg.clusters.push_back(s);
    }

    cfg.map = MapConfig{};
    const QJsonObject mo = o.value("map").toObject();
    if (!mo.isEmpty()) {
        cfg.map.startOffline = mo.value("start_offline").toBool(false);
        cfg.map.offlineTilesDir = mo.value("offline_tiles_dir").toString().trimmed();
        cfg.map.cacheDir = mo.value("cache_dir").toString().trimmed();

        const QString ua = mo.value("user_agent").toString().trimmed();
        if (!ua.isEmpty()) cfg.map.userAgent = ua;

        cfg.map.probeIntervalMs = mo.value("probe_interval_ms").toInt(0);
        cfg.map.probeUrl = mo.value("probe_url").toString().trimmed();
        cfg.map.probeTimeoutMs = mo.value("probe_timeout_ms").toInt(0);
    }

    if (cfg.map.cacheDir.isEmpty()) cfg.map.cacheDir = defaultMapCacheDir();

    return cfg;
}

QJsonObject AppConfig::toRunConfigPatch(const QString& deviceMode) const {
    QJsonObject patch;

    QJsonObject dev;
    dev.insert("device_mode", deviceMode);
    dev.insert("fallback_to_cpu", true);
    patch.insert("device", dev);

    patch.insert("yolo_dir", yoloDir);
    patch.insert("yolo_model_path", yoloModelPath);

    return patch;
}