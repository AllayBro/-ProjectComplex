#include "FullDistanceTab.h"

#include "RunnerClient.h"
#include "ResultView.h"

#include <QScrollArea>
#include <QVBoxLayout>
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
#include <QCryptographicHash>
static QString uiIniPathFull() {
    return QDir(QCoreApplication::applicationDirPath()).filePath("ui.ini");
}

static QString uiDefaultImagesDirFull() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (d.isEmpty()) d = QDir::homePath();
    return d;
}

static QString uiDefaultOutDirFull() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
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

static QByteArray sha256OfFile(const QString& path, QString* errorText = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorText) *errorText = "Не удалось открыть файл модели: " + path;
        return {};
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        const QByteArray chunk = f.read(1024 * 1024);
        if (chunk.isEmpty() && f.error() != QFile::NoError) {
            if (errorText) *errorText = "Ошибка чтения файла модели: " + path;
            return {};
        }
        hash.addData(chunk);
    }
    return hash.result();
}

static QString findSameModelByContent(const QString& sourcePath, const QString& yoloDir, QString* errorText = nullptr) {
    const QByteArray srcHash = sha256OfFile(sourcePath, errorText);
    if (srcHash.isEmpty()) return {};

    QDir d(yoloDir);
    const QStringList files = d.entryList(QStringList() << "*.pt" << "*.onnx", QDir::Files, QDir::Name);
    for (const QString& fn : files) {
        const QString abs = d.filePath(fn);
        QString hashErr;
        const QByteArray curHash = sha256OfFile(abs, &hashErr);
        if (!hashErr.isEmpty()) {
            if (errorText) *errorText = hashErr;
            return {};
        }
        if (curHash == srcHash) {
            return QDir::cleanPath(QFileInfo(abs).absoluteFilePath());
        }
    }

    return {};
}

static QString uniqueModelTargetPath(const QString& yoloDir, const QString& sourcePath) {
    const QFileInfo fi(sourcePath);
    const QString base = fi.completeBaseName();
    const QString ext = fi.suffix();

    QString candidate = QDir(yoloDir).filePath(fi.fileName());
    if (!QFileInfo::exists(candidate)) {
        return QDir::cleanPath(candidate);
    }

    for (int i = 2; ; ++i) {
        const QString fileName = ext.isEmpty()
            ? QString("%1_%2").arg(base).arg(i)
            : QString("%1_%2.%3").arg(base).arg(i).arg(ext);

        candidate = QDir(yoloDir).filePath(fileName);
        if (!QFileInfo::exists(candidate)) {
            return QDir::cleanPath(candidate);
        }
    }
}

static QString importYoloModelToDir(const QString& selectedPath, const QString& yoloDir, QString* errorText = nullptr) {
    if (selectedPath.trimmed().isEmpty()) {
        if (errorText) *errorText = "Файл модели не выбран.";
        return {};
    }

    const QString srcAbs = QDir::cleanPath(QFileInfo(selectedPath).absoluteFilePath());
    if (!QFileInfo(srcAbs).exists()) {
        if (errorText) *errorText = "Файл модели не найден: " + srcAbs;
        return {};
    }

    if (isPathInsideDirOrUnknown(srcAbs, yoloDir)) {
        return srcAbs;
    }

    QString dupErr;
    const QString sameExisting = findSameModelByContent(srcAbs, yoloDir, &dupErr);
    if (!dupErr.isEmpty()) {
        if (errorText) *errorText = dupErr;
        return {};
    }
    if (!sameExisting.isEmpty()) {
        return sameExisting;
    }

    const QString dstAbs = uniqueModelTargetPath(yoloDir, srcAbs);
    if (!QFile::copy(srcAbs, dstAbs)) {
        if (errorText) *errorText = "Не удалось скопировать модель в папку yolo: " + dstAbs;
        return {};
    }

    return dstAbs;
}

static void selectYoloInCombo(QComboBox* combo, const QString& absPath) {
    if (!combo) return;

    const QString target = QDir::cleanPath(QFileInfo(absPath).absoluteFilePath());
    const QString targetName = QFileInfo(target).fileName();

    for (int i = 0; i < combo->count(); ++i) {
        const QString dataPath = QDir::cleanPath(combo->itemData(i).toString());
        const QString textName = combo->itemText(i).trimmed();

#ifdef Q_OS_WIN
        const bool sameData = (QString::compare(dataPath, target, Qt::CaseInsensitive) == 0);
        const bool sameName = (QString::compare(textName, targetName, Qt::CaseInsensitive) == 0);
#else
        const bool sameData = (dataPath == target);
        const bool sameName = (textName == targetName);
#endif

        if (sameData || sameName) {
            combo->setCurrentIndex(i);
            return;
        }
    }

    combo->setEditText(target);
}

FullDistanceTab::FullDistanceTab(const AppConfig& cfg, const QString& appDir, QWidget* parent)
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

    m_run = new QPushButton("Полный режим");
    m_run->setFixedHeight(actionBtnH);

    m_view = new ResultView();
    m_view->setPythonExe(m_cfg.pythonExe);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_view);

    root->addLayout(row1);
    root->addLayout(row2);
    root->addLayout(row3);
    root->addWidget(m_run);
    root->addWidget(scroll, 1);

    {
        QSettings s(uiIniPathFull(), QSettings::IniFormat);

        const QString lastIn = s.value("ui/last_input_path", "").toString();

        const QString lastYolo = s.value("ui/last_yolo_model_path", m_cfg.yoloModelPath).toString();

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
    }

    connect(m_browse, &QPushButton::clicked, this, [this]{
        QSettings s(uiIniPathFull(), QSettings::IniFormat);
        QString startDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (startDir.isEmpty()) startDir = QDir::homePath();

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

    connect(m_input, &QLineEdit::textChanged, this, [this](const QString& t){
        const QString p = t.trimmed();
        if (p.isEmpty()) return;

        QFileInfo fi(p);
        if (!fi.exists() || !fi.isFile()) return;

        QSettings s(uiIniPathFull(), QSettings::IniFormat);
        s.setValue("ui/last_input_path", fi.absoluteFilePath());
        s.setValue("ui/last_image_dir", fi.absolutePath());
        s.sync();

        applyPreview(fi.absoluteFilePath());
        emit imageSelected(fi.absoluteFilePath());
    });
    connect(m_browseYolo, &QPushButton::clicked, this, [this]{
        QString yoloErr;
        if (!m_cfg.ensureYoloDirExists(m_appDir, &yoloErr)) {
            QMessageBox::warning(this, "YOLO", yoloErr);
            return;
        }

        const QString ydir = yoloDirAbs();
        QSettings s(uiIniPathFull(), QSettings::IniFormat);

        QString startDir = s.value("ui/last_yolo_import_dir", ydir).toString().trimmed();
        const QString current = currentYoloModelPath();
        if (!current.isEmpty() && QFileInfo(current).exists()) {
            startDir = QFileInfo(current).absolutePath();
        }

        const QStringList selected = QFileDialog::getOpenFileNames(
            this,
            "Выберите модели YOLO",
            startDir,
            "YOLO weights (*.pt *.onnx)"
        );
        if (selected.isEmpty()) return;

        s.setValue("ui/last_yolo_import_dir", QFileInfo(selected.last()).absolutePath());

        QStringList imported;
        QStringList failed;

        for (const QString& one : selected) {
            QString importErr;
            const QString importedPath = importYoloModelToDir(one, ydir, &importErr);
            if (importedPath.isEmpty()) {
                failed << (QFileInfo(one).fileName() + ": " +
                           (importErr.isEmpty() ? "не удалось подключить модель." : importErr));
                continue;
            }

    #ifdef Q_OS_WIN
            if (!imported.contains(importedPath, Qt::CaseInsensitive)) imported << importedPath;
    #else
            if (!imported.contains(importedPath)) imported << importedPath;
    #endif
        }

        if (imported.isEmpty()) {
            QMessageBox::warning(
                this,
                "YOLO",
                failed.isEmpty() ? "Не удалось подключить модели YOLO." : failed.join("\n")
            );
            return;
        }

        reloadYoloModels();

        if (m_yoloModel) {
            QSignalBlocker b(m_yoloModel);
            selectYoloInCombo(m_yoloModel, imported.last());
        }

        s.setValue("ui/last_yolo_model_path", imported.last());
        s.sync();

        if (!failed.isEmpty()) {
            QMessageBox::warning(this, "YOLO", failed.join("\n"));
        }
    });

    connect(m_yoloModel, &QComboBox::currentTextChanged, this, [this](const QString& t){
        const QString p = t.trimmed();
        if (!p.isEmpty()) {
            QSettings s(uiIniPathFull(), QSettings::IniFormat);
            s.setValue("ui/last_yolo_model_path", p);
            s.sync();
        }
    });

    connect(m_run, &QPushButton::clicked, this, [this]{
        if (m_runner && m_runner->isRunning()) {
            m_view->appendLog("Уже выполняется. Дождитесь завершения или остановите запуск.");
            return;
        }
        const QString in = m_input->text().trimmed();
        const QString dev = m_device->currentText();

        QSettings s(uiIniPathFull(), QSettings::IniFormat);

        const QString out;

        const QString yolo = currentYoloModelPath();

        if (in.isEmpty()) {
            m_view->appendLog("Ошибка: задайте фото.");
            return;
        }
        QString yoloPath = yolo;
        if (yoloPath.isEmpty() || !QFileInfo(yoloPath).exists()) {
            m_browseYolo->click();
            yoloPath = currentYoloModelPath();
            if (yoloPath.isEmpty() || !QFileInfo(yoloPath).exists()) {
                return;
            }
        }

        const QString ydir = yoloDirAbs();
        if (!isPathInsideDirOrUnknown(yoloPath, ydir)) {
            QMessageBox::warning(this, "YOLO", "Модель должна быть внутри папки yolo.");
            return;
        }


        s.setValue("ui/last_input_path", in);
        s.setValue("ui/last_image_dir", QFileInfo(in).absolutePath());
        s.setValue("ui/last_yolo_model_path", yoloPath);
        s.sync();

        m_lastRunImagePath = in;
        m_view->clearRunKeepPreview();
        m_runner->runFullDistance(in, out, dev, yoloPath);
    });

    bindRunner();
}

QString FullDistanceTab::yoloDirAbs() const {
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

void FullDistanceTab::reloadYoloModels() {
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

QString FullDistanceTab::currentYoloModelPath() const {
    if (!m_yoloModel) return {};

    const QString dataPath = m_yoloModel->currentData().toString().trimmed();
    if (!dataPath.isEmpty()) {
        return QDir::cleanPath(QFileInfo(dataPath).absoluteFilePath());
    }

    QString t = m_yoloModel->currentText().trimmed();
    if (t.isEmpty()) return {};

    const QString ydir = yoloDirAbs();
    QFileInfo fi(t);

    if (!fi.isAbsolute()) {
        if (!ydir.isEmpty()) {
            return QDir::cleanPath(QDir(ydir).filePath(t));
        }
        return {};
    }

    return QDir::cleanPath(fi.absoluteFilePath());
}

static QString absPathLocalFull(const QString& appDir, const QString& relOrAbs) {
    QFileInfo fi(relOrAbs);
    if (fi.isAbsolute()) return fi.absoluteFilePath();
    return QDir(appDir).filePath(relOrAbs);
}

static QString tempBaseDirLocalFull() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (d.isEmpty()) d = QDir::tempPath();
    if (d.isEmpty()) d = QDir::homePath();
    return d;
}

bool FullDistanceTab::runPreviewTaskRaw(const QString& inputPath, QImage& outImage, QJsonObject& outExifRoot, QString& err) {
    outImage = QImage();
    outExifRoot = QJsonObject();
    err.clear();

    const QString py = m_cfg.pythonExe.trimmed().isEmpty() ? "python" : m_cfg.pythonExe.trimmed();
    const QString runner = absPathLocalFull(m_appDir, m_cfg.runnerScript);
    const QString basePyCfg = absPathLocalFull(m_appDir, m_cfg.pythonConfigJson);

    if (!QFileInfo(runner).exists()) { err = "runner.py не найден: " + runner; return false; }
    if (!QFileInfo(basePyCfg).exists()) { err = "python config не найден: " + basePyCfg; return false; }
    if (!QFileInfo(inputPath).exists()) { err = "input не найден: " + inputPath; return false; }

    const QString workDir = QDir(tempBaseDirLocalFull()).filePath("traffic_preview_raw");
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

void FullDistanceTab::applyPreview(const QString& imagePath) {
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

void FullDistanceTab::bindRunner() {
    connect(m_runner, &RunnerClient::started, this, [this]{
        m_browse->setEnabled(false);
        m_input->setEnabled(false);
        m_yoloModel->setEnabled(false);
        m_browseYolo->setEnabled(false);
        m_device->setEnabled(false);
        m_run->setEnabled(false);

        m_view->appendLog("Запуск...");
    });

    connect(m_runner, &RunnerClient::logLine, this, [this](const QString& s){
        m_view->appendLog(s);
    });

    connect(m_runner, &RunnerClient::finishedError, this, [this](const QString& e){
        m_browse->setEnabled(true);
        m_input->setEnabled(true);
        m_yoloModel->setEnabled(true);
        m_browseYolo->setEnabled(true);
        m_device->setEnabled(true);
        m_run->setEnabled(true);

        m_view->appendLog("Ошибка: " + e);
    });

    connect(m_runner, &RunnerClient::finishedOk, this, [this](const ModuleResult& r){
        m_browse->setEnabled(true);
        m_input->setEnabled(true);
        m_yoloModel->setEnabled(true);
        m_browseYolo->setEnabled(true);
        m_device->setEnabled(true);
        m_run->setEnabled(true);

        m_view->setResult(r);
        emit resultReady(m_lastRunImagePath, r);
    });
}