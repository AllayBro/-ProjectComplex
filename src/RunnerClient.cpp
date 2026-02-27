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
#include <algorithm>
#include <functional>

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

    QDir().mkpath(outputDir);

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

    startProcess(args);
}

void RunnerClient::runFullDistance(const QString& imagePath,
                                   const QString& outputDir,
                                   const QString& deviceMode,
                                   const QString& yoloModelPath) {
    const QString runner = absPath(m_appDir, m_cfg.runnerScript);
    const QString basePyCfg = absPath(m_appDir, m_cfg.pythonConfigJson);

    QDir().mkpath(outputDir);

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

    startProcess(args);
}

void RunnerClient::startProcess(const QStringList& args) {
    m_stdoutBuf.clear();
    m_stderrBuf.clear();

    if (m_proc) {
        m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }

    m_proc = new QProcess(this);
    m_proc->setWorkingDirectory(m_appDir);

    connect(m_proc, &QProcess::started, this, [this]{ emit started(); });
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &RunnerClient::onReadyStdout);
    connect(m_proc, &QProcess::readyReadStandardError,  this, &RunnerClient::onReadyStderr);
    connect(m_proc, &QProcess::errorOccurred,           this, &RunnerClient::onError);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &RunnerClient::onFinished);

    m_proc->setProgram(m_cfg.pythonExe);

    QStringList fullArgs;
    fullArgs << "-u";
    fullArgs << args;
    m_proc->setArguments(fullArgs);

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

void RunnerClient::onError(QProcess::ProcessError) {
    const QString s = m_proc ? m_proc->errorString() : QString("Unknown process error");
    emit finishedError("Process error: " + s);
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
        const QString outDir = QFileInfo(m_resultJsonPath).absolutePath();
        QString csvPath;
        QString cerr;
        if (!writeDetectionsCsv(outDir, r, csvPath, cerr)) emit logLine("CSV export error: " + cerr);
        emit finishedOk(r);
    }

    if (failed) {
        emit finishedError(QString("Process failed: exitCode=%1 status=%2").arg(exitCode).arg((int)status));
        return;
    }

    if (!hasResult) {
        emit finishedError(perr.isEmpty() ? "No result.json produced" : perr);
        return;
    }
}