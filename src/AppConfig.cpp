#include "AppConfig.h"

#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QStandardPaths>
#include <QStringList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

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


static bool isWindowsStorePythonAlias(const QString& path) {
#ifdef Q_OS_WIN
    const QString p = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()).toLower();
    return p.contains("/appdata/local/microsoft/windowsapps/");
#else
    Q_UNUSED(path);
    return false;
#endif
}

static QString findBundledPython(const QString& appDirPath) {
#ifdef Q_OS_WIN
    const QStringList candidates = {
        "py/traffic_py.exe",
        "py/pythonw.exe",
        "py/python.exe"
    };
#else
    const QStringList candidates = {
        "py/bin/python3",
        "py/bin/python"
    };
#endif

    for (const QString& rel : candidates) {
        const QString abs = QDir(appDirPath).filePath(rel);
        const QFileInfo fi(abs);
        if (fi.exists() && fi.isFile()) {
            return QDir::cleanPath(fi.absoluteFilePath());
        }
    }

    return {};
}

AppConfig AppConfig::loadOrDie(const QString& appDirPath) {
    AppConfig cfg;

    const QString path = QDir(appDirPath).filePath("config/app_config.json");
    const QJsonObject o = readJsonObjectOrDie(path);

    cfg.pythonExe = o.value("python_exe").toString("python").trimmed();
    if (cfg.pythonExe.isEmpty()) cfg.pythonExe = "python";

    const QString pyKey = cfg.pythonExe.toLower();
    if (pyKey == "python" || pyKey == "python.exe") {
        const QString bundledPython = findBundledPython(appDirPath);
        if (!bundledPython.isEmpty()) {
            cfg.pythonExe = bundledPython;
        } else {
            QString found = QStandardPaths::findExecutable("python");
            if (isWindowsStorePythonAlias(found)) {
                found.clear();
            }
            if (!found.isEmpty()) {
                cfg.pythonExe = QDir::cleanPath(QFileInfo(found).absoluteFilePath());
            }
        }
    }

    cfg.runnerScript = o.value("runner_script").toString("python/runner.py").trimmed();
    if (cfg.runnerScript.isEmpty()) cfg.runnerScript = "python/runner.py";

    cfg.pythonConfigJson = o.value("python_config_json").toString("python/default_config.json").trimmed();
    if (cfg.pythonConfigJson.isEmpty()) cfg.pythonConfigJson = "python/default_config.json";

    cfg.yoloDir = o.value("yolo_dir").toString("yolo").trimmed();
    if (cfg.yoloDir.isEmpty()) cfg.yoloDir = "yolo";

    cfg.yoloModelPath = o.value("yolo_model_path").toString("yolo11n.pt").trimmed();
    if (cfg.yoloModelPath.isEmpty()) cfg.yoloModelPath = "yolo11n.pt";
    QString yoloErr;
    if (!cfg.ensureYoloDirExists(appDirPath, &yoloErr)) {
        qWarning().noquote() << yoloErr;
    }
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

    // Нормализуем пути карты относительно каталога приложения
    QDir baseDir(appDirPath);

    if (!cfg.map.offlineTilesDir.isEmpty() && !QDir::isAbsolutePath(cfg.map.offlineTilesDir)) {
        cfg.map.offlineTilesDir = baseDir.filePath(cfg.map.offlineTilesDir);
    }

    if (!cfg.map.cacheDir.isEmpty() && !QDir::isAbsolutePath(cfg.map.cacheDir)) {
        cfg.map.cacheDir = baseDir.filePath(cfg.map.cacheDir);
    }

    if (cfg.map.cacheDir.isEmpty()) cfg.map.cacheDir = defaultMapCacheDir();
    if (cfg.map.probeUrl.isEmpty()) cfg.map.probeUrl = "https://tile.openstreetmap.org/0/0/0.png";
    if (cfg.map.probeIntervalMs <= 0) cfg.map.probeIntervalMs = 30000;
    if (cfg.map.probeTimeoutMs <= 0) cfg.map.probeTimeoutMs = 5000;

    return cfg;
}

QString AppConfig::yoloDirAbsolute(const QString& appDirPath) const {
    QFileInfo fi(yoloDir);
    if (fi.isAbsolute()) return QDir::cleanPath(fi.absoluteFilePath());
    return QDir(appDirPath).filePath(yoloDir);
}

bool AppConfig::ensureYoloDirExists(const QString& appDirPath, QString* errorText) const {
    const QString dirPath = yoloDirAbsolute(appDirPath);
    if (dirPath.trimmed().isEmpty()) {
        if (errorText) *errorText = "Путь yolo_dir пуст.";
        return false;
    }

    QDir d(dirPath);
    if (d.exists()) return true;

    if (QDir().mkpath(dirPath)) return true;

    if (errorText) *errorText = "Не удалось создать папку YOLO: " + dirPath;
    return false;
}

QString AppConfig::normalizeDeviceMode(const QString& deviceMode) const {
    const QString v = deviceMode.trimmed().toLower();
    if (v == "gpu" || v == "cpu" || v == "auto") return v;
    return "auto";
}

bool AppConfig::isGpuAvailable(const QString& appDirPath, QString* detail) const {
    if (detail) detail->clear();

    QString pyExe = pythonExe.trimmed();
    if (pyExe.isEmpty()) pyExe = "python";

    const bool hasDirSep = pyExe.contains('/') || pyExe.contains('\\');
    if (QFileInfo(pyExe).isAbsolute()) {
        pyExe = QDir::cleanPath(QFileInfo(pyExe).absoluteFilePath());
    } else if (hasDirSep) {
        pyExe = QDir::cleanPath(QDir(appDirPath).filePath(pyExe));
    }

    static QString s_cacheKey;
    static QString s_cacheDetail;
    static bool s_cacheReady = false;
    static bool s_cacheResult = false;

    const QString cacheKey = QDir::cleanPath(appDirPath) + "|" + pyExe;
    if (s_cacheReady && s_cacheKey == cacheKey) {
        if (detail) *detail = s_cacheDetail;
        return s_cacheResult;
    }

    auto rememberResult = [&](bool ok, const QString& info) {
        s_cacheKey = cacheKey;
        s_cacheDetail = info;
        s_cacheResult = ok;
        s_cacheReady = true;
        if (detail) *detail = info;
        return ok;
    };

    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONUTF8", "1");
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("PYTHONFAULTHANDLER", "1");

    const QFileInfo pyFi(pyExe);
    if (pyFi.isAbsolute() && pyFi.exists() && pyFi.isFile()) {
        env.insert("PYTHONHOME", QDir::toNativeSeparators(pyFi.absolutePath()));
    }
    proc.setProcessEnvironment(env);

    const QString probeCode = QString::fromUtf8(R"PY(
import json
try:
    import torch
    cuda_ok = bool(torch.cuda.is_available())
    device_count = int(torch.cuda.device_count()) if cuda_ok else 0
    print(json.dumps({"cuda": cuda_ok, "count": device_count}, ensure_ascii=False))
except Exception as e:
    print(json.dumps({"cuda": False, "count": 0, "error": str(e)}, ensure_ascii=False))
)PY").trimmed();

    proc.start(pyExe, QStringList() << "-c" << probeCode);
    if (!proc.waitForStarted(5000)) {
        return rememberResult(false, QStringLiteral("Проверка GPU не выполнена: Python не запустился."));
    }
    if (!proc.waitForFinished(10000)) {
        proc.kill();
        proc.waitForFinished(1000);
        return rememberResult(false, QStringLiteral("Проверка GPU не завершилась вовремя."));
    }

    const QString stdoutText = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    const QString stderrText = QString::fromUtf8(proc.readAllStandardError()).trimmed();

    QString jsonLine;
    const QStringList lines = stdoutText.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
    if (!lines.isEmpty()) jsonLine = lines.constLast().trimmed();

    if (jsonLine.isEmpty()) {
        return rememberResult(
            false,
            stderrText.isEmpty()
                ? QStringLiteral("GPU не обнаружен.")
                : QStringLiteral("GPU не обнаружен: ") + stderrText
        );
    }

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonLine.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        return rememberResult(
            false,
            stderrText.isEmpty()
                ? QStringLiteral("GPU не обнаружен.")
                : QStringLiteral("GPU не обнаружен: ") + stderrText
        );
    }

    const QJsonObject obj = doc.object();
    const bool cudaOk = obj.value("cuda").toBool(false);
    const int count = obj.value("count").toInt(0);
    const QString err = obj.value("error").toString().trimmed();

    if (cudaOk && count > 0) {
        return rememberResult(true, QString("GPU доступен: %1").arg(count));
    }

    return rememberResult(
        false,
        err.isEmpty()
            ? QStringLiteral("GPU не обнаружен.")
            : QStringLiteral("GPU недоступен: ") + err
    );
}

QString AppConfig::effectiveDeviceMode(const QString& appDirPath, const QString& requestedMode, QString* note) const {
    if (note) note->clear();

    const QString normalized = normalizeDeviceMode(requestedMode);
    if (normalized != "gpu") return normalized;

    QString gpuDetail;
    if (isGpuAvailable(appDirPath, &gpuDetail)) return "gpu";

    if (note) {
        *note = gpuDetail.isEmpty()
            ? "Режим gpu недоступен на этом ПК. Использую cpu."
            : gpuDetail + " Использую cpu.";
    }
    return "cpu";
}

QJsonObject AppConfig::toRunConfigPatch(const QString& deviceMode) const {
    QJsonObject patch;

    QJsonObject dev;
    dev.insert("device_mode", normalizeDeviceMode(deviceMode));
    dev.insert("fallback_to_cpu", true);
    patch.insert("device", dev);

    patch.insert("yolo_dir", yoloDir);
    patch.insert("yolo_model_path", yoloModelPath);

    return patch;
}