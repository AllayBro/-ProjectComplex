#include "RunnerClient.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QDateTime>

RunnerClient::RunnerClient(const AppConfig& cfg, const QString& appDirPath, QObject* parent)
    : QObject(parent), m_cfg(cfg), m_appDir(appDirPath) {}

QString RunnerClient::absPath(const QString& appDir, const QString& relOrAbs) {
    QFileInfo fi(relOrAbs);
    if (fi.isAbsolute()) return fi.absoluteFilePath();
    return QDir(appDir).filePath(relOrAbs);
}

void RunnerClient::runCluster(int clusterId, const QString& imagePath, const QString& outputDir, const QString& deviceMode) {
    QString runner = absPath(m_appDir, m_cfg.runnerScript);
    QString pyCfg  = absPath(m_appDir, m_cfg.pythonConfigJson);

    QDir().mkpath(outputDir);

    QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    m_resultJsonPath = QDir(outputDir).filePath(QString("result_cluster_%1_%2.json").arg(clusterId).arg(ts));

    QStringList args;
    args << runner
         << "--task" << "cluster"
         << "--cluster-id" << QString::number(clusterId)
         << "--input" << imagePath
         << "--output-dir" << outputDir
         << "--device" << deviceMode
         << "--config" << pyCfg
         << "--result-json" << m_resultJsonPath;

    startProcess(args);
}

void RunnerClient::runFullDistance(const QString& imagePath, const QString& outputDir, const QString& deviceMode) {
    QString runner = absPath(m_appDir, m_cfg.runnerScript);
    QString pyCfg  = absPath(m_appDir, m_cfg.pythonConfigJson);

    QDir().mkpath(outputDir);

    QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    m_resultJsonPath = QDir(outputDir).filePath(QString("result_full_%1.json").arg(ts));

    QStringList args;
    args << runner
         << "--task" << "distance_full"
         << "--input" << imagePath
         << "--output-dir" << outputDir
         << "--device" << deviceMode
         << "--config" << pyCfg
         << "--result-json" << m_resultJsonPath;

    startProcess(args);
}

void RunnerClient::startProcess(const QStringList& args) {
    if (m_proc) {
        m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }

    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &RunnerClient::onReadyStdout);
    connect(m_proc, &QProcess::readyReadStandardError, this, &RunnerClient::onReadyStderr);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &RunnerClient::onFinished);

    emit started();

    QString py = m_cfg.pythonExe;
    m_proc->setProgram(py);
    m_proc->setArguments(args);

    m_proc->start();
}

void RunnerClient::onReadyStdout() {
    QByteArray b = m_proc->readAllStandardOutput();
    QString s = QString::fromUtf8(b);
    for (const QString& line : s.split('\n')) {
        if (!line.trimmed().isEmpty()) emit logLine(line.trimmed());
    }
}

void RunnerClient::onReadyStderr() {
    QByteArray b = m_proc->readAllStandardError();
    QString s = QString::fromUtf8(b);
    for (const QString& line : s.split('\n')) {
        if (!line.trimmed().isEmpty()) emit logLine(line.trimmed());
    }
}

bool RunnerClient::loadResultJson(const QString& path, ModuleResult& out, QString& err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { err = "Cannot open result json: " + path; return false; }
    QByteArray data = f.readAll();
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError) { err = "Result json parse error: " + pe.errorString(); return false; }
    if (!doc.isObject()) { err = "Result json is not object"; return false; }

    QJsonObject o = doc.object();
    return ModuleResult::fromJson(o, out, err);
}

void RunnerClient::onFinished(int exitCode, QProcess::ExitStatus status) {
    if (status != QProcess::NormalExit || exitCode != 0) {
        QString err = QString("Process failed: exitCode=%1 status=%2").arg(exitCode).arg((int)status);
        emit finishedError(err);
        return;
    }

    ModuleResult r;
    QString perr;
    if (!loadResultJson(m_resultJsonPath, r, perr)) {
        emit finishedError(perr);
        return;
    }
    emit finishedOk(r);
}
