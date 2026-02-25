#pragma once
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include "AppConfig.h"

struct ModuleResult;

class RunnerClient : public QObject {
    Q_OBJECT
public:
    explicit RunnerClient(const AppConfig& cfg, const QString& appDirPath, QObject* parent = nullptr);
    void appendLog(const QString& s);
    void runCluster(int clusterId,
                    const QString& imagePath,
                    const QString& outputDir,
                    const QString& deviceMode);

    void runFullDistance(const QString& imagePath,
                         const QString& outputDir,
                         const QString& deviceMode);

    signals:
        void started();
    void logLine(const QString& line);
    void finishedOk(const ModuleResult& result);
    void finishedError(const QString& errorText);

private slots:
    void onReadyStdout();
    void onReadyStderr();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError(QProcess::ProcessError err);

private:
    AppConfig m_cfg;
    QString m_appDir;
    QProcess* m_proc = nullptr;

    QString m_resultJsonPath;

    void startProcess(const QStringList& args);
    static bool loadResultJson(const QString& path, ModuleResult& out, QString& err);
    static QString absPath(const QString& appDir, const QString& relOrAbs);

    static bool writeDetectionsCsv(const QString& outDir,
                                  ModuleResult& r,
                                  QString& csvPath,
                                  QString& err);
};