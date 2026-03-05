#include "RunnerClient.h"

#include "ModelTypes.h"

#include <QJsonParseError>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDateTime>
#include <QSet>
#include <QTextStream>
#include <QStringConverter>
#include <QProcessEnvironment>
#include <functional>
#include <algorithm>
#include <QMap>
#include <QtMath>
#include <cstring>
#include <QStandardPaths>

static QString quoteForLog(QString s) {
    if (s.contains('\"')) s.replace('\"', "\\\"");
    const bool need = s.contains(' ') || s.contains('\t') || s.contains('\n') || s.contains('\r');
    return need ? ('\"' + s + '\"') : s;
}

static QString processErrorToText(QProcess::ProcessError e) {
    switch (e) {
    case QProcess::FailedToStart: return "FailedToStart";
    case QProcess::Crashed:       return "Crashed";
    case QProcess::Timedout:      return "Timedout";
    case QProcess::ReadError:     return "ReadError";
    case QProcess::WriteError:    return "WriteError";
    case QProcess::UnknownError:  return "UnknownError";
    }
    return "UnknownError";
}

static QString exitStatusToText(QProcess::ExitStatus s) {
    switch (s) {
    case QProcess::NormalExit: return "NormalExit";
    case QProcess::CrashExit:  return "CrashExit";
    }
    return "CrashExit";
}



static QString tempBaseDir() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (d.isEmpty()) d = QDir::tempPath();
    if (d.isEmpty()) d = QDir::homePath();
    return d;
}

static bool ensureEmptyDir(const QString& dirPath, QString& err) {
    QDir d(dirPath);
    if (d.exists()) {
        if (!d.removeRecursively()) {
            err = "Cannot clear temp_dir: " + dirPath;
            return false;
        }
    }
    if (!QDir().mkpath(dirPath)) {
        err = "Cannot create temp_dir: " + dirPath;
        return false;
    }
    return true;
}

static QString makeWorkDir(const QString& subDir, QString& err) {
    const QString root = QDir(tempBaseDir()).filePath("traffic_work");
    const QString full = QDir(root).filePath(subDir);
    if (!ensureEmptyDir(full, err)) return {};
    return full;
}

static QString canonDirPath(const QString& p) {
    QString c = QDir(p).canonicalPath();
    if (c.isEmpty()) c = QDir(p).absolutePath();
    return QDir::cleanPath(c);
}

static bool isInsideDir(const QString& child, const QString& parent) {
    const QString c = canonDirPath(child);
    const QString p = canonDirPath(parent);
    if (c.isEmpty() || p.isEmpty()) return false;
    if (QString::compare(c, p, Qt::CaseInsensitive) == 0) return true;
    const QString pref = p + QDir::separator();
    return c.startsWith(pref, Qt::CaseInsensitive);
}

static void cleanupWorkDirIfTemp(const QString& dirPath) {
    if (dirPath.trimmed().isEmpty()) return;
    const QString root = QDir(tempBaseDir()).filePath("traffic_work");
    if (!isInsideDir(dirPath, root)) return;
    QDir(dirPath).removeRecursively();
}

static bool writeJsonObjectFile(const QString& path, const QJsonObject& obj, QString& err) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { err = "Cannot write json: " + path; return false; }
    QJsonDocument doc(obj);
    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

static bool readJsonObjectFile(const QString& path, QJsonObject& out, QString& err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        err = "Cannot open json: " + path;
        return false;
    }
    const QByteArray data = f.readAll();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError) {
        err = "JSON parse error in " + path + ": " + pe.errorString();
        return false;
    }
    if (!doc.isObject()) {
        err = "JSON is not object: " + path;
        return false;
    }

    out = doc.object();
    return true;
}

static void deepMergeInto(QJsonObject& target, const QJsonObject& patch) {
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const QString key = it.key();
        const QJsonValue val = it.value();

        if (val.isObject() && target.value(key).isObject()) {
            QJsonObject nested = target.value(key).toObject();
            deepMergeInto(nested, val.toObject());
            target.insert(key, nested);
        } else {
            target.insert(key, val);
        }
    }
}

static void consumeTextLines(QString& buffer, QString chunk, const std::function<void(const QString&)>& emitFn) {
    chunk.replace('\r', '\n');
    buffer += chunk;

    int nl = buffer.indexOf('\n');
    while (nl >= 0) {
        QString line = buffer.left(nl);
        buffer.remove(0, nl + 1);

        if (!line.trimmed().isEmpty()) emitFn(line);
        nl = buffer.indexOf('\n');
    }
}

static void flushTail(QString& buffer, const std::function<void(const QString&)>& emitFn) {
    if (!buffer.trimmed().isEmpty()) emitFn(buffer);
    buffer.clear();
}

static QString resolveExistingDirUp(const QString& startDir, const QString& relOrAbs) {
    QFileInfo fi(relOrAbs);
    if (fi.isAbsolute()) return QDir::cleanPath(fi.absoluteFilePath());

    QDir curDir(QDir(startDir).absolutePath());
    while (true) {
        const QString cand = QDir(curDir.absolutePath()).filePath(relOrAbs);
        if (QDir(cand).exists()) return QDir::cleanPath(QDir(cand).absolutePath());

        const QString before = curDir.absolutePath();
        if (!curDir.cdUp()) break;
        const QString after = curDir.absolutePath();
        if (after == before) break;
    }

    return QDir(startDir).filePath(relOrAbs);
}

RunnerClient::RunnerClient(const AppConfig& cfg, const QString& appDirPath, QObject* parent)
    : QObject(parent), m_cfg(cfg), m_appDir(appDirPath) {}

void RunnerClient::appendLog(const QString& s) {
    if (!s.trimmed().isEmpty()) emit logLine(s);
}

QString RunnerClient::absPath(const QString& appDir, const QString& relOrAbs) {
    QFileInfo fi(relOrAbs);
    if (fi.isAbsolute()) return fi.absoluteFilePath();
    return QDir(appDir).filePath(relOrAbs);
}

void RunnerClient::runCluster(int clusterId,
                              const QString& imagePath,
                              const QString& outputDir,
                              const QString& deviceMode,
                              const QString& yoloModelPath) {
    const QString runner = absPath(m_appDir, m_cfg.runnerScript);
    const QString basePyCfg = absPath(m_appDir, m_cfg.pythonConfigJson);

    QString outErr;
    const QString workDir = makeWorkDir(QString("cluster_%1").arg(clusterId), outErr);
    if (workDir.isEmpty()) { emit finishedError(outErr); return; }

    m_resultJsonPath = QDir(workDir).filePath("result.json");
    QString yoloAbs = yoloModelPath.trimmed();
    if (yoloAbs.isEmpty()) { emit finishedError("Не выбрана модель YOLO."); return; }
    if (!QFileInfo(yoloAbs).isAbsolute()) yoloAbs = absPath(m_appDir, yoloAbs);
    if (!QFileInfo(yoloAbs).exists()) { emit finishedError("Файл модели YOLO не найден: " + yoloAbs); return; }

    QJsonObject cfgObj;
    QString cfgErr;
    if (!readJsonObjectFile(basePyCfg, cfgObj, cfgErr)) { emit finishedError(cfgErr); return; }

    QJsonObject patch = m_cfg.toRunConfigPatch(deviceMode);
    patch.insert("yolo_dir", resolveExistingDirUp(m_appDir, m_cfg.yoloDir));
    patch.insert("yolo_model_path", yoloAbs);
    deepMergeInto(cfgObj, patch);

    const QString patchedCfg = QDir(workDir).filePath("run_cfg.json");
    if (!writeJsonObjectFile(patchedCfg, cfgObj, cfgErr)) { emit finishedError(cfgErr); return; }

    QStringList args;
    args << runner
         << "--task" << "cluster"
         << "--cluster-id" << QString::number(clusterId)
         << "--input" << imagePath
         << "--output-dir" << workDir
         << "--device" << deviceMode
         << "--config" << patchedCfg
         << "--result-json" << m_resultJsonPath;

    m_currentOutDir = workDir;
    startProcess(args);
}

void RunnerClient::runFullDistance(const QString& imagePath,
                                   const QString& outputDir,
                                   const QString& deviceMode,
                                   const QString& yoloModelPath) {
    const QString runner = absPath(m_appDir, m_cfg.runnerScript);
    const QString basePyCfg = absPath(m_appDir, m_cfg.pythonConfigJson);

    QString outErr;
    const QString workDir = makeWorkDir("full_distance", outErr);
    if (workDir.isEmpty()) { emit finishedError(outErr); return; }

    m_resultJsonPath = QDir(workDir).filePath("result.json");
    QString yoloAbs = yoloModelPath.trimmed();
    if (yoloAbs.isEmpty()) { emit finishedError("Не выбрана модель YOLO."); return; }
    if (!QFileInfo(yoloAbs).isAbsolute()) yoloAbs = absPath(m_appDir, yoloAbs);
    if (!QFileInfo(yoloAbs).exists()) { emit finishedError("Файл модели YOLO не найден: " + yoloAbs); return; }

    QJsonObject cfgObj;
    QString cfgErr;
    if (!readJsonObjectFile(basePyCfg, cfgObj, cfgErr)) { emit finishedError(cfgErr); return; }

    QJsonObject patch = m_cfg.toRunConfigPatch(deviceMode);
    patch.insert("yolo_dir", resolveExistingDirUp(m_appDir, m_cfg.yoloDir));
    patch.insert("yolo_model_path", yoloAbs);
    deepMergeInto(cfgObj, patch);

    const QString patchedCfg = QDir(workDir).filePath("run_cfg.json");
    if (!writeJsonObjectFile(patchedCfg, cfgObj, cfgErr)) { emit finishedError(cfgErr); return; }

    QStringList args;
    args << runner
         << "--task" << "distance_full"
         << "--input" << imagePath
         << "--output-dir" << workDir
         << "--device" << deviceMode
         << "--config" << patchedCfg
         << "--result-json" << m_resultJsonPath;

    m_currentOutDir = workDir;
    startProcess(args);
}
void RunnerClient::startProcess(const QStringList& args) {
    m_stdoutBuf.clear();
    m_stderrBuf.clear();

    m_cancelRequested = false;
    m_finishEmitted = false;
    m_lastCmdLine.clear();

    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        emit finishedError("Process already running");
        return;
    }

    if (!m_proc) {
        m_proc = new QProcess(this);
        connect(m_proc, &QProcess::started, this, [this]{ emit started(); });
        connect(m_proc, &QProcess::readyReadStandardOutput, this, &RunnerClient::onReadyStdout);
        connect(m_proc, &QProcess::readyReadStandardError,  this, &RunnerClient::onReadyStderr);
        connect(m_proc, &QProcess::errorOccurred,           this, &RunnerClient::onError);
        connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &RunnerClient::onFinished);
    }

    const QString runnerPath = args.isEmpty() ? QString() : args.first();
    const QString runnerDir  = runnerPath.isEmpty() ? QString() : QFileInfo(runnerPath).absolutePath();
    if (!runnerDir.isEmpty() && QDir(runnerDir).exists()) m_proc->setWorkingDirectory(runnerDir);
    else m_proc->setWorkingDirectory(m_appDir);

    m_runLog.close();
    m_runLogPath.clear();

    const QString pyExe = absPath(m_appDir, m_cfg.pythonExe);
    m_proc->setProgram(pyExe);

    QStringList fullArgs;
    fullArgs << "-u";
    fullArgs << "-X" << "faulthandler";
    fullArgs << args;
    m_proc->setArguments(fullArgs);

    // Команда (для диагностики)
    {
        QStringList parts;
        parts << quoteForLog(pyExe);
        for (const auto& a : fullArgs) parts << quoteForLog(a);
        m_lastCmdLine = parts.join(' ');
    }

    // Окружение: стабильная кодировка + PYTHONPATH + faulthandler
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PYTHONUTF8", "1");
        env.insert("PYTHONIOENCODING", "utf-8");
        env.insert("PYTHONFAULTHANDLER", "1");

        const QString sep = QString(QDir::listSeparator());
        QStringList pp;
        if (!runnerDir.isEmpty()) pp << runnerDir;
        pp << m_appDir;

        const QString existing = env.value("PYTHONPATH");
        if (!existing.isEmpty()) {
            const QStringList parts = existing.split(QDir::listSeparator(), Qt::SkipEmptyParts);
            for (const auto& p : parts) pp << p;
        }
        env.insert("PYTHONPATH", pp.join(sep));
        env.insert("PYTHONHOME", QDir::toNativeSeparators(QFileInfo(pyExe).absolutePath()));
        m_proc->setProcessEnvironment(env);
    }

    m_proc->start();
}

void RunnerClient::onReadyStdout() {
    if (!m_proc) return;
    const QByteArray b = m_proc->readAllStandardOutput();
    consumeTextLines(m_stdoutBuf, QString::fromUtf8(b), [this](const QString& line){ emit logLine(line); });
}

void RunnerClient::onReadyStderr() {
    if (!m_proc) return;
    const QByteArray b = m_proc->readAllStandardError();
    consumeTextLines(m_stderrBuf, QString::fromUtf8(b), [this](const QString& line){ emit logLine(line); });
}

void RunnerClient::onError(QProcess::ProcessError err) {
    if (m_finishEmitted) return;

    const QString es = m_proc ? m_proc->errorString() : QString("Unknown error");
    const QString et = processErrorToText(err);

    // Для FailedToStart finished может не дать нормального stderr, фиксируем сразу.
    if (err == QProcess::FailedToStart) {
        m_finishEmitted = true;
        if (m_runLog.isOpen()) m_runLog.close();
        emit finishedError(QString("Process error: %1 (%2) | %3").arg(es).arg(et).arg(m_lastCmdLine));
        return;
    }

    // Остальные ошибки (Crashed и т.п.) добиваем на onFinished, без двойного “Ошибка: …”
    emit logLine(QString("ERROR: %1 (%2)").arg(es).arg(et));
}

bool RunnerClient::loadResultJson(const QString& path, ModuleResult& out, QString& err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { err = "Cannot open result json: " + path; return false; }

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) { err = "Result json parse error: " + pe.errorString(); return false; }
    if (!doc.isObject()) { err = "Result json is not object"; return false; }

    return ModuleResult::fromJson(doc.object(), out, err);
}

static QString jsonToCompact(const QJsonValue& v) {
    if (v.isNull() || v.isUndefined()) return {};
    if (v.isString()) return v.toString();
    if (v.isBool()) return v.toBool() ? "true" : "false";
    if (v.isDouble()) return QString::number(v.toDouble(), 'f', 6);
    if (v.isArray()) {
        QJsonDocument d(v.toArray());
        return QString::fromUtf8(d.toJson(QJsonDocument::Compact));
    }
    if (v.isObject()) {
        QJsonDocument d(v.toObject());
        return QString::fromUtf8(d.toJson(QJsonDocument::Compact));
    }
    return {};
}

static QString csvEscape(const QString& s) {
    QString out = s;
    if (out.contains('\"')) out.replace('\"', "\"\"");
    const bool needQuotes = out.contains(',') || out.contains('\n') || out.contains('\r') || out.contains('\"');
    if (needQuotes) out = '\"' + out + '\"';
    return out;
}

bool RunnerClient::writeDetectionsCsv(const QString& outDir, ModuleResult& r, QString& csvPath, QString& err) {
    QSet<QString> metaKeys;
    metaKeys.reserve(64);
    for (const auto& d : r.detections) {
        for (auto it = d.meta.begin(); it != d.meta.end(); ++it) metaKeys.insert(it.key());
    }

    QStringList metaSorted = metaKeys.values();
    std::sort(metaSorted.begin(), metaSorted.end());

    QStringList cols;
    cols << "id" << "cls" << "conf" << "w_px" << "h_px";
    cols << metaSorted;

    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    csvPath = QDir(outDir).filePath(QString("detections_%1_%2.csv").arg(r.moduleId).arg(ts));

    QFile f(csvPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        err = "Cannot write csv: " + csvPath;
        return false;
    }

    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);

    {
        QStringList header;
        header.reserve(cols.size());
        for (const auto& c : cols) header << csvEscape(c);
        out << header.join(',') << "\n";
    }

    for (int i = 0; i < r.detections.size(); ++i) {
        const auto& d = r.detections[i];
        const int w = d.x2 - d.x1;
        const int h = d.y2 - d.y1;

        QStringList row;
        row.reserve(cols.size());
        row << csvEscape(QString::number(i + 1));
        row << csvEscape(d.clsName);
        row << csvEscape(QString::number(d.conf, 'f', 6));
        row << csvEscape(QString::number(w));
        row << csvEscape(QString::number(h));

        for (const auto& k : metaSorted) {
            row << csvEscape(jsonToCompact(d.meta.value(k)));
        }

        out << row.join(',') << "\n";
    }

    f.close();

    r.artifacts.insert("csv_path", csvPath);
    return true;
}

void RunnerClient::onFinished(int exitCode, QProcess::ExitStatus status)
{
    flushTail(m_stdoutBuf, [this](const QString& line){ emit logLine(line); });
    flushTail(m_stderrBuf, [this](const QString& line){ emit logLine(line); });

    const bool failed = (status != QProcess::NormalExit || exitCode != 0);

    ModuleResult r;
    QString perr;
    const bool hasResult = QFileInfo(m_resultJsonPath).exists() && loadResultJson(m_resultJsonPath, r, perr);

    const QString outToCleanup = m_currentOutDir;
    m_currentOutDir.clear();

    if (hasResult) {
        emit finishedOk(r);
        cleanupWorkDirIfTemp(outToCleanup);
        return;
    }

    if (failed) {
        QString e = QString("Process failed: exitCode=%1 status=%2").arg(exitCode).arg((int)status);
        if (!m_lastCmdLine.isEmpty()) e += " | " + m_lastCmdLine;
        emit finishedError(e);
        cleanupWorkDirIfTemp(outToCleanup);
        return;
    }

    emit finishedError(perr.isEmpty() ? "No result.json produced" : perr);
    cleanupWorkDirIfTemp(outToCleanup);
}

RunnerClient::~RunnerClient() {
    stop();
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(1500);
    }
    if (m_runLog.isOpen()) m_runLog.close();
}

bool RunnerClient::isRunning() const {
    return m_proc && m_proc->state() != QProcess::NotRunning;
}

void RunnerClient::stop() {
    m_cancelRequested = true;
    if (!m_proc) return;
    if (m_proc->state() == QProcess::NotRunning) return;
    m_proc->kill();
}