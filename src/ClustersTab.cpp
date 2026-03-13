#include "ClustersTab.h"

#include "RunnerClient.h"
#include "ResultView.h"

#include <QScrollArea>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDir>
#include <QLabel>
#include <QFileInfo>
#include <QCoreApplication>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QFrame>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QFile>
#include <QMessageBox>

static QString uiIniPathClusters() {
    return QDir(QCoreApplication::applicationDirPath()).filePath("ui.ini");
}

static QString uiDefaultImagesDirClusters() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (d.isEmpty()) d = QDir::homePath();
    return d;
}

static QString normPathSlash(const QString& p) {
    return QDir::cleanPath(QDir::fromNativeSeparators(p));
}

static bool isPathInsideDirOrUnknown(const QString& filePath, const QString& dirPath) {
    if (filePath.isEmpty() || dirPath.isEmpty()) return false;

    QString d = QDir(dirPath).canonicalPath();
    if (d.isEmpty()) d = QFileInfo(dirPath).absoluteFilePath();

    QString f = QFileInfo(filePath).canonicalFilePath();
    if (f.isEmpty()) f = QFileInfo(filePath).absoluteFilePath();

    if (d.isEmpty() || f.isEmpty()) return true; // не удалось нормализовать, не блокируем

    d = normPathSlash(d);
    f = normPathSlash(f);

    if (!d.endsWith('/')) d += '/';

#ifdef Q_OS_WIN
    return f.startsWith(d, Qt::CaseInsensitive);
#else
    return f.startsWith(d);
#endif
}


ClustersTab::ClustersTab(const AppConfig& cfg, const QString& appDir, QWidget* parent)
    : QWidget(parent), m_cfg(cfg), m_appDir(appDir) {

    m_runner = new RunnerClient(cfg, appDir, this);
    const int topCtrlH = 34;
    const int actionBtnH = 42;
    QString yoloErr;
    m_cfg.ensureYoloDirExists(m_appDir, &yoloErr);

    auto* root = new QVBoxLayout(this);

    auto* row1 = new QHBoxLayout();
    m_input = new QLineEdit();
    m_input->setFixedHeight(topCtrlH);

    m_browse = new QPushButton("Выбрать фото");
    m_browse->setFixedHeight(topCtrlH);
    row1->addWidget(m_input, 1);
    row1->addWidget(m_browse);

    auto* row2 = new QHBoxLayout();
    m_yoloModel = new QComboBox();
    m_yoloModel->setEditable(true);
    m_yoloModel->setFixedHeight(topCtrlH);

    m_browseYolo = new QPushButton("Модель YOLO");
    m_browseYolo->setFixedHeight(topCtrlH);

    row2->addWidget(m_yoloModel, 1);
    row2->addWidget(m_browseYolo);
    auto* row3 = new QHBoxLayout();
    row3->setContentsMargins(0, 0, 0, 0);
    row3->setSpacing(8);

    auto* deviceLabel = new QLabel("Device:");
    deviceLabel->setFixedHeight(topCtrlH);
    deviceLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    m_device = new QComboBox();
    m_device->addItems({"auto", "gpu", "cpu"});
    m_device->setFixedSize(140, topCtrlH);

    row3->addWidget(deviceLabel);
    row3->addWidget(m_device, 0, Qt::AlignLeft | Qt::AlignVCenter);
    row3->addStretch(1);

    auto* rowBtns = new QHBoxLayout();
    for (const auto& c : m_cfg.clusters) {
        auto* b = new QPushButton(QString::number(c.clusterId) + ": " + c.title);
        b->setFixedHeight(actionBtnH);
        m_clusterButtons.push_back(b);
        rowBtns->addWidget(b);
    }

    m_view = new ResultView();
    m_view->setPythonExe(m_cfg.pythonExe);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_view);

    root->addLayout(row1);
    root->addLayout(row2);
    root->addLayout(row3);
    root->addLayout(rowBtns);
    root->addWidget(scroll, 1);

    {
        QSettings s(uiIniPathClusters(), QSettings::IniFormat);

        const QString lastYolo = s.value("ui/last_yolo_model_path", m_cfg.yoloModelPath).toString();
        const QString lastIn = s.value("ui/last_input_path", "").toString();

        reloadYoloModels();
        if (m_yoloModel) {
            QSignalBlocker b(m_yoloModel);
            m_yoloModel->setEditText(lastYolo);
        }
        if (!lastIn.isEmpty() && QFileInfo(lastIn).exists()) {
            QSignalBlocker b(m_input);
            m_input->setText(lastIn);
            applyPreview(lastIn);
            emit imageSelected(lastIn);
        }

        s.sync();
    }

    connect(m_browse, &QPushButton::clicked, this, [this] {
        QSettings s(uiIniPathClusters(), QSettings::IniFormat);

        QString startDir = s.value("ui/last_image_dir", uiDefaultImagesDirClusters()).toString();
        const QString cur = m_input->text().trimmed();
        if (!cur.isEmpty()) {
            QFileInfo fi(cur);
            if (fi.exists()) startDir = fi.absolutePath();
        }

        const QString p = QFileDialog::getOpenFileName(
            this,
            "Выберите изображение",
            startDir,
            "Images (*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff *.heic *.heif)"
        );

        if (!p.isEmpty()) {
            s.setValue("ui/last_image_dir", QFileInfo(p).absolutePath());
            s.setValue("ui/last_input_path", p);
            s.sync();

            QSignalBlocker b(m_input);
            m_input->setText(p);
            applyPreview(p);
            emit imageSelected(p);
        }
    });

    connect(m_input, &QLineEdit::textChanged, this, [this](const QString& t) {
        const QString p = t.trimmed();
        if (p.isEmpty()) return;

        QFileInfo fi(p);
        if (!fi.exists() || !fi.isFile()) return;

        QSettings s(uiIniPathClusters(), QSettings::IniFormat);
        s.setValue("ui/last_input_path", fi.absoluteFilePath());
        s.setValue("ui/last_image_dir", fi.absolutePath());
        s.sync();

        applyPreview(fi.absoluteFilePath());
        emit imageSelected(fi.absoluteFilePath());
    });
    connect(m_browseYolo, &QPushButton::clicked, this, [this] {
        QString yoloErr;
        if (!m_cfg.ensureYoloDirExists(m_appDir, &yoloErr)) {
            QMessageBox::warning(this, "YOLO", "YOLO не установлен.");
            return;
        }

        reloadYoloModels();

        const QString ydir = yoloDirAbs();
        const QStringList models = QDir(ydir).entryList(QStringList() << "*.pt" << "*.onnx", QDir::Files, QDir::Name);
        if (models.isEmpty()) {
            QMessageBox::warning(this, "YOLO", "YOLO не установлен.");
            return;
        }

        const QString p = QFileDialog::getOpenFileName(
            this,
            "Выберите модель YOLO",
            ydir,
            "YOLO weights (*.pt *.onnx)"
        );
        if (p.isEmpty()) return;

        if (!isPathInsideDirOrUnknown(p, ydir)) {
            QMessageBox::warning(this, "YOLO", "Модель должна быть внутри папки yolo.");
            return;
        }

        if (m_yoloModel) {
            QSignalBlocker b(m_yoloModel);
            m_yoloModel->setEditText(p);
        }

        QSettings s(uiIniPathClusters(), QSettings::IniFormat);
        s.setValue("ui/last_yolo_model_path", p);
        s.sync();
    });

    connect(m_browseYolo, &QPushButton::clicked, this, [this] {
        QString yoloErr;
        if (!m_cfg.ensureYoloDirExists(m_appDir, &yoloErr)) {
            QMessageBox::warning(this, "YOLO", "YOLO не установлен.");
            return;
        }

        reloadYoloModels();

        const QString ydir = yoloDirAbs();
        const QStringList models = QDir(ydir).entryList(QStringList() << "*.pt" << "*.onnx", QDir::Files, QDir::Name);
        if (models.isEmpty()) {
            QMessageBox::warning(this, "YOLO", "YOLO не установлен.");
            return;
        }

        const QString p = QFileDialog::getOpenFileName(
            this,
            "Выберите модель YOLO",
            ydir,
            "YOLO weights (*.pt *.onnx)"
        );
        if (p.isEmpty()) return;

        if (!isPathInsideDirOrUnknown(p, ydir)) {
            QMessageBox::warning(this, "YOLO", "Модель должна быть внутри папки yolo.");
            return;
        }

        if (m_yoloModel) {
            QSignalBlocker b(m_yoloModel);
            m_yoloModel->setEditText(p);
        }

        QSettings s(uiIniPathClusters(), QSettings::IniFormat);
        s.setValue("ui/last_yolo_model_path", p);
        s.sync();
    });

    for (int i = 0; i < m_clusterButtons.size(); ++i) {
        const int clusterId = m_cfg.clusters[i].clusterId;
        connect(m_clusterButtons[i], &QPushButton::clicked, this, [this, clusterId] {
            if (m_runner && m_runner->isRunning()) {
                m_view->logEdit()->append("Уже выполняется. Дождитесь завершения или остановите запуск.");
                return;
            }
            const QString in = m_input->text().trimmed();
            const QString dev = m_device->currentText();

            const QString out;

            const QString yolo = currentYoloModelPath();
            if (yolo.isEmpty() || !QFileInfo(yolo).exists()) {
                QMessageBox::warning(this, "YOLO", "YOLO не установлен.");
                return;
            }

            const QString ydir = yoloDirAbs();
            if (!isPathInsideDirOrUnknown(yolo, ydir)) {
                QMessageBox::warning(this, "YOLO", "Модель должна быть внутри папки yolo.");
                return;
            }

            m_lastRunImagePath = in;

            m_view->clearRunKeepPreview();
            m_runner->runCluster(clusterId, in, out, dev, yolo);
        });
    }

    bindRunner();
}

QString ClustersTab::yoloDirAbs() const {
    QDir curDir(QDir(m_appDir).absolutePath());
    while (true) {
        const QString cand = QDir(curDir.absolutePath()).filePath(m_cfg.yoloDir);
        if (QDir(cand).exists()) return QDir::cleanPath(QDir(cand).absolutePath());

        const QString before = curDir.absolutePath();
        if (!curDir.cdUp()) break;
        const QString after = curDir.absolutePath();
        if (after == before) break;
    }
    return m_cfg.yoloDirAbsolute(m_appDir);
}

void ClustersTab::reloadYoloModels() {
    if (!m_yoloModel) return;

    const QString currentText = m_yoloModel->currentText().trimmed();

    QString yoloErr;
    m_cfg.ensureYoloDirExists(m_appDir, &yoloErr);

    const QString ydir = yoloDirAbs();
    QDir d(ydir);

    QStringList files;
    if (d.exists()) {
        files = d.entryList(QStringList() << "*.pt" << "*.onnx", QDir::Files, QDir::Name);
    }

    QSignalBlocker b(m_yoloModel);
    m_yoloModel->clear();
    for (const QString& fn : files) {
        const QString abs = d.filePath(fn);
        m_yoloModel->addItem(fn, abs);
    }

    if (!currentText.isEmpty()) {
        m_yoloModel->setEditText(currentText);
    } else if (m_yoloModel->count() > 0) {
        m_yoloModel->setCurrentIndex(0);
    }
}

QString ClustersTab::currentYoloModelPath() const {
    if (!m_yoloModel) return {};

    QString t = m_yoloModel->currentText().trimmed();
    if (t.isEmpty()) return {};

    const QString ydir = yoloDirAbs();
    QFileInfo fi(t);

    if (!fi.isAbsolute()) {
        if (!ydir.isEmpty()) return QDir::cleanPath(QDir(ydir).filePath(t));
        return {};
    }
    return fi.absoluteFilePath();
}


static QString absPathLocal(const QString& appDir, const QString& relOrAbs) {
    QFileInfo fi(relOrAbs);
    if (fi.isAbsolute()) return fi.absoluteFilePath();
    return QDir(appDir).filePath(relOrAbs);
}

static QString tempBaseDirLocal() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (d.isEmpty()) d = QDir::tempPath();
    if (d.isEmpty()) d = QDir::homePath();
    return d;
}

bool ClustersTab::runPreviewTaskRaw(const QString& inputPath, QImage& outImage, QJsonObject& outExifRoot, QString& err) {
    outImage = QImage();
    outExifRoot = QJsonObject();
    err.clear();

    const QString py = m_cfg.pythonExe.trimmed().isEmpty() ? "python" : m_cfg.pythonExe.trimmed();
    const QString runner = absPathLocal(m_appDir, m_cfg.runnerScript);
    const QString basePyCfg = absPathLocal(m_appDir, m_cfg.pythonConfigJson);

    if (!QFileInfo(runner).exists()) { err = "runner.py не найден: " + runner; return false; }
    if (!QFileInfo(basePyCfg).exists()) { err = "python config не найден: " + basePyCfg; return false; }
    if (!QFileInfo(inputPath).exists()) { err = "input не найден: " + inputPath; return false; }

    const QString workDir = QDir(tempBaseDirLocal()).filePath("traffic_preview_raw");
    QDir wd(workDir);
    if (wd.exists() && !wd.removeRecursively()) { err = "Не удалось очистить temp preview dir: " + workDir; return false; }
    if (!QDir().mkpath(workDir)) { err = "Не удалось создать temp preview dir: " + workDir; return false; }

    const QString resultJson = QDir(workDir).filePath("preview.json");

    QProcess proc;
    QStringList args;
    args << "-u" << "-X" << "faulthandler"
         << runner
         << "--task" << "preview"
         << "--input" << inputPath
         << "--output-dir" << workDir
         << "--device" << (m_device ? m_device->currentText() : QString("auto"))
         << "--config" << basePyCfg
         << "--result-json" << resultJson;

    proc.start(py, args);
    if (!proc.waitForFinished(-1)) {
        err = "preview: процесс не завершился";
        wd.removeRecursively();
        return false;
    }

    if (!QFileInfo(resultJson).exists()) {
        err = "preview: result_json не создан. stderr=" + QString::fromUtf8(proc.readAllStandardError()).trimmed();
        wd.removeRecursively();
        return false;
    }

    QFile f(resultJson);
    if (!f.open(QIODevice::ReadOnly)) { err = "preview: не открыть result_json: " + resultJson; wd.removeRecursively(); return false; }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        err = "preview: JSON parse error: " + pe.errorString();
        wd.removeRecursively();
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject mod = root.value("module").toObject();

    const int w = mod.value("preview_w").toInt(0);
    const int h = mod.value("preview_h").toInt(0);
    const int stride = mod.value("preview_stride").toInt(0);
    const QString fmt = mod.value("preview_format").toString().trimmed();
    const QString rawPath = mod.value("preview_raw_path").toString().trimmed();

    if (w <= 0 || h <= 0 || stride <= 0) { err = "preview: неверные размеры"; wd.removeRecursively(); return false; }
    if (fmt != "RGBA8888") { err = "preview: неподдерживаемый формат: " + fmt; wd.removeRecursively(); return false; }
    if (rawPath.isEmpty() || !QFileInfo(rawPath).exists()) { err = "preview: raw файл не найден: " + rawPath; wd.removeRecursively(); return false; }

    QFile rf(rawPath);
    if (!rf.open(QIODevice::ReadOnly)) { err = "preview: не открыть raw: " + rawPath; wd.removeRecursively(); return false; }
    const QByteArray bytes = rf.readAll();
    rf.close();

    const qint64 need = (qint64)stride * (qint64)h;
    if (bytes.size() < need) { err = "preview: raw меньше ожидаемого"; wd.removeRecursively(); return false; }

    QImage img((const uchar*)bytes.constData(), w, h, stride, QImage::Format_RGBA8888);
    outImage = img.copy();

    outExifRoot = root.value("exif").toObject();

    wd.removeRecursively();
    return !outImage.isNull();
}

void ClustersTab::applyPreview(const QString& imagePath) {
    const QString p = imagePath.trimmed();
    if (p.isEmpty() || !QFileInfo(p).exists()) return;

    QPixmap pm(p);
    if (!pm.isNull()) {
        m_view->setPreviewImage(p);
        return;
    }

    QImage img;
    QJsonObject exifRoot;
    QString err;
    if (!runPreviewTaskRaw(p, img, exifRoot, err)) {
        m_view->setPreviewImage(p);
        if (m_view && m_view->logEdit()) m_view->logEdit()->append("HEIC/HEIF preview error: " + err);
        return;
    }

    m_view->setPreviewFromRaw(p, img, exifRoot);
}

void ClustersTab::bindRunner() {
    connect(m_runner, &RunnerClient::started, this, [this] {
        m_browse->setEnabled(false);
        m_input->setEnabled(false);
        m_yoloModel->setEnabled(false);
        m_browseYolo->setEnabled(false);
        m_device->setEnabled(false);
        for (auto* b : m_clusterButtons) b->setEnabled(false);

        m_view->logEdit()->append("Запуск...");
    });

    connect(m_runner, &RunnerClient::logLine, this, [this](const QString& s) {
        m_view->logEdit()->append(s);
    });

    connect(m_runner, &RunnerClient::finishedError, this, [this](const QString& e) {
        m_browse->setEnabled(true);
        m_input->setEnabled(true);
        m_yoloModel->setEnabled(true);
        m_browseYolo->setEnabled(true);
        m_device->setEnabled(true);
        for (auto* b : m_clusterButtons) b->setEnabled(true);

        m_view->logEdit()->append("Ошибка: " + e);
    });

    connect(m_runner, &RunnerClient::finishedOk, this, [this](const ModuleResult& r) {
        m_browse->setEnabled(true);
        m_input->setEnabled(true);
        m_yoloModel->setEnabled(true);
        m_browseYolo->setEnabled(true);
        m_device->setEnabled(true);
        for (auto* b : m_clusterButtons) b->setEnabled(true);

        m_view->setResult(r);
        emit resultReady(m_lastRunImagePath, r);
    });
}