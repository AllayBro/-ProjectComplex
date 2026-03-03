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



static bool readJsonObjectFile(const QString& path, QJsonObject& out, QString& err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { err = "Cannot open json: " + path; return false; }

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) { err = "JSON parse error in " + path + ": " + pe.errorString(); return false; }
    if (!doc.isObject()) { err = "JSON is not object: " + path; return false; }

    out = doc.object();
    return true;
}

static bool writeJsonObjectFile(const QString& path, const QJsonObject& obj, QString& err) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { err = "Cannot write json: " + path; return false; }
    QJsonDocument doc(obj);
    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

static void deepMergeInto(QJsonObject& dst, const QJsonObject& src) {
    for (auto it = src.begin(); it != src.end(); ++it) {
        const QString k = it.key();
        const QJsonValue sv = it.value();

        if (sv.isObject() && dst.value(k).isObject()) {
            QJsonObject dchild = dst.value(k).toObject();
            deepMergeInto(dchild, sv.toObject());
            dst.insert(k, dchild);
        } else {
            dst.insert(k, sv);
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

static bool resetDirIfUnderRunRoot(const QString& appDir, const QString& outDir, QString& err) {
    const QString runRootAbs = QDir(appDir).filePath("run");

    const QString rootCanon = QDir(runRootAbs).canonicalPath();
    QString outCanon = QDir(outDir).canonicalPath();

    const QString root = rootCanon.isEmpty() ? QDir(runRootAbs).absolutePath() : rootCanon;
    const QString out  = outCanon.isEmpty() ? QDir(outDir).absolutePath() : outCanon;

    const bool underRun = (!root.isEmpty()) && (out == root || out.startsWith(root + QDir::separator()));

    if (underRun) {
        QDir d(outDir);
        if (d.exists()) {
            if (!d.removeRecursively()) {
                err = "Cannot clear run_dir: " + outDir;
                return false;
            }
        }
        if (!QDir().mkpath(outDir)) {
            err = "Cannot create run_dir: " + outDir;
            return false;
        }
        return true;
    }

    if (!QDir().mkpath(outDir)) {
        err = "Cannot create output_dir: " + outDir;
        return false;
    }

    return true;
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
    if (!resetDirIfUnderRunRoot(m_appDir, outputDir, outErr)) { emit finishedError(outErr); return; }

    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    m_resultJsonPath = QDir(outputDir).filePath(QString("result_cluster_%1_%2.json").arg(clusterId).arg(ts));

    QString yoloAbs = yoloModelPath.trimmed();
    if (yoloAbs.isEmpty()) { emit finishedError("Не выбрана модель YOLO."); return; }
    if (!QFileInfo(yoloAbs).isAbsolute()) yoloAbs = absPath(m_appDir, yoloAbs);
    if (!QFileInfo(yoloAbs).exists()) { emit finishedError("Файл модели YOLO не найден: " + yoloAbs); return; }

    QJsonObject cfgObj;
    QString cfgErr;
    if (!readJsonObjectFile(basePyCfg, cfgObj, cfgErr)) { emit finishedError(cfgErr); return; }

    QJsonObject patch = m_cfg.toRunConfigPatch(deviceMode);
    patch.insert("yolo_dir", absPath(m_appDir, m_cfg.yoloDir));
    patch.insert("yolo_model_path", yoloAbs);
    deepMergeInto(cfgObj, patch);

    const QString patchedCfg = QDir(outputDir).filePath(QString("run_cfg_cluster_%1_%2.json").arg(clusterId).arg(ts));
    if (!writeJsonObjectFile(patchedCfg, cfgObj, cfgErr)) { emit finishedError(cfgErr); return; }

    QStringList args;
    args << runner
         << "--task" << "cluster"
         << "--cluster-id" << QString::number(clusterId)
         << "--input" << imagePath
         << "--output-dir" << outputDir
         << "--device" << deviceMode
         << "--config" << patchedCfg
         << "--result-json" << m_resultJsonPath;

    m_currentOutDir = outputDir;
    startProcess(args);
}

void RunnerClient::runFullDistance(const QString& imagePath,
                                   const QString& outputDir,
                                   const QString& deviceMode,
                                   const QString& yoloModelPath) {
    const QString runner = absPath(m_appDir, m_cfg.runnerScript);
    const QString basePyCfg = absPath(m_appDir, m_cfg.pythonConfigJson);

    QString outErr;
    if (!resetDirIfUnderRunRoot(m_appDir, outputDir, outErr)) { emit finishedError(outErr); return; }

    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    m_resultJsonPath = QDir(outputDir).filePath(QString("result_full_%1.json").arg(ts));

    QString yoloAbs = yoloModelPath.trimmed();
    if (yoloAbs.isEmpty()) { emit finishedError("Не выбрана модель YOLO."); return; }
    if (!QFileInfo(yoloAbs).isAbsolute()) yoloAbs = absPath(m_appDir, yoloAbs);
    if (!QFileInfo(yoloAbs).exists()) { emit finishedError("Файл модели YOLO не найден: " + yoloAbs); return; }

    QJsonObject cfgObj;
    QString cfgErr;
    if (!readJsonObjectFile(basePyCfg, cfgObj, cfgErr)) { emit finishedError(cfgErr); return; }

    QJsonObject patch = m_cfg.toRunConfigPatch(deviceMode);
    patch.insert("yolo_dir", absPath(m_appDir, m_cfg.yoloDir));
    patch.insert("yolo_model_path", yoloAbs);
    deepMergeInto(cfgObj, patch);

    const QString patchedCfg = QDir(outputDir).filePath(QString("run_cfg_full_%1.json").arg(ts));
    if (!writeJsonObjectFile(patchedCfg, cfgObj, cfgErr)) { emit finishedError(cfgErr); return; }

    QStringList args;
    args << runner
         << "--task" << "distance_full"
         << "--input" << imagePath
         << "--output-dir" << outputDir
         << "--device" << deviceMode
         << "--config" << patchedCfg
         << "--result-json" << m_resultJsonPath;

    m_currentOutDir = outputDir;
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

    // Лог процесса в output-dir
    m_runLog.close();
    m_runLogPath.clear();
    if (!m_currentOutDir.trimmed().isEmpty()) {
        const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
        m_runLogPath = QDir(m_currentOutDir).filePath(QString("process_%1.log").arg(ts));
        m_runLog.setFileName(m_runLogPath);
        if (!m_runLog.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit logLine("WARNING: cannot open run log file: " + m_runLogPath);
        }
    }

    m_proc->setProgram(m_cfg.pythonExe);

    QStringList fullArgs;
    fullArgs << "-u";
    fullArgs << "-X" << "faulthandler";
    fullArgs << args;
    m_proc->setArguments(fullArgs);

    // Команда (для диагностики)
    {
        QStringList parts;
        parts << quoteForLog(m_cfg.pythonExe);
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

        m_proc->setProcessEnvironment(env);
    }

    m_proc->start();
}

void RunnerClient::onReadyStdout() {
    if (!m_proc) return;
    const QByteArray b = m_proc->readAllStandardOutput();
    if (m_runLog.isOpen() && !b.isEmpty()) { m_runLog.write(b); m_runLog.flush(); }
    consumeTextLines(m_stdoutBuf, QString::fromUtf8(b), [this](const QString& line){ emit logLine(line); });
}

void RunnerClient::onReadyStderr() {
    if (!m_proc) return;
    const QByteArray b = m_proc->readAllStandardError();
    if (m_runLog.isOpen() && !b.isEmpty()) { m_runLog.write(b); m_runLog.flush(); }
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

void RunnerClient::onFinished(int exitCode, QProcess::ExitStatus status) {
    flushTail(m_stdoutBuf, [this](const QString& line){ emit logLine(line); });
    flushTail(m_stderrBuf, [this](const QString& line){ emit logLine(line); });

    const bool failed = (status != QProcess::NormalExit || exitCode != 0);

    ModuleResult r;
    QString perr;
    const bool hasResult = QFileInfo(m_resultJsonPath).exists() && loadResultJson(m_resultJsonPath, r, perr);

    if (hasResult) {
        emit finishedOk(r);
        // Если результат есть, считаем выполнение успешным даже при ненулевом exitCode.
        return;
    }

    if (failed) {
        QString e = QString("Process failed: exitCode=%1 status=%2").arg(exitCode).arg((int)status);
        if (!m_lastCmdLine.isEmpty()) e += " | " + m_lastCmdLine;
        emit finishedError(e);
        return;
    }

    emit finishedError(perr.isEmpty() ? "No result.json produced" : perr);
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