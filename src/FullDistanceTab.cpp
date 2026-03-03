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

FullDistanceTab::FullDistanceTab(const AppConfig& cfg, const QString& appDir, QWidget* parent)
    : QWidget(parent), m_cfg(cfg), m_appDir(appDir) {

    m_runner = new RunnerClient(cfg, appDir, this);

    auto* root = new QVBoxLayout(this);

    auto* row1 = new QHBoxLayout();
    m_input = new QLineEdit();
    m_browse = new QPushButton("Выбрать фото");
    row1->addWidget(m_input, 1);
    row1->addWidget(m_browse);

    auto* row2 = new QHBoxLayout();
    m_yoloModel = new QComboBox();
    m_yoloModel->setEditable(true);
    m_browseYolo = new QPushButton("Модель YOLO");
    row2->addWidget(m_yoloModel, 1);
    row2->addWidget(m_browseYolo);

    auto* row3 = new QHBoxLayout();
    m_device = new QComboBox();
    m_device->addItems({"auto", "gpu", "cpu"});
    row3->addWidget(new QLabel("Device:"));
    row3->addWidget(m_device);

    m_run = new QPushButton("Запуск полного режима (детекция → расстояние → CSV → очистка)");
    m_run->setMinimumHeight(44);

    m_view = new ResultView();

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
            m_view->setPreviewImage(lastIn);
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
        const QString out = QDir(QDir(m_appDir).filePath("run")).filePath("full_distance");

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
            m_view->setPreviewImage(p);
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

        m_view->setPreviewImage(fi.absoluteFilePath());
        emit imageSelected(fi.absoluteFilePath());
    });

    connect(m_browseYolo, &QPushButton::clicked, this, [this]{
        const QString ydir = yoloDirAbs();
        if (ydir.isEmpty() || !QDir(ydir).exists()) {
            m_view->appendLog("Ошибка: папка yolo_dir не найдена: " + ydir);
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
            m_view->appendLog("Ошибка: модель должна быть внутри папки: " + ydir);
            return;
        }

        if (m_yoloModel) {
            QSignalBlocker b(m_yoloModel);
            m_yoloModel->setEditText(p);
        }

        QSettings s(uiIniPathFull(), QSettings::IniFormat);
        s.setValue("ui/last_yolo_model_path", p);
        s.sync();
    });

    connect(m_yoloModel, &QComboBox::currentTextChanged, this, [this](const QString& t){
        const QString p = t.trimmed();
        if (p.isEmpty()) return;

        QSettings s(uiIniPathFull(), QSettings::IniFormat);
        s.setValue("ui/last_yolo_model_path", p);
        s.sync();
    });

    connect(m_run, &QPushButton::clicked, this, [this]{
        if (m_runner && m_runner->isRunning()) {
            m_view->appendLog("Уже выполняется. Дождитесь завершения или остановите запуск.");
            return;
        }
        const QString in = m_input->text().trimmed();
        const QString dev = m_device->currentText();

        QSettings s(uiIniPathFull(), QSettings::IniFormat);

        QString out = s.value("ui/last_output_dir", uiDefaultOutDirFull()).toString();
        if (out.isEmpty()) {
            out = uiDefaultOutDirFull();
            s.setValue("ui/last_output_dir", out);
        }

        const QString yolo = currentYoloModelPath();

        if (in.isEmpty()) {
            m_view->appendLog("Ошибка: задайте фото.");
            return;
        }
        if (yolo.isEmpty() || !QFileInfo(yolo).exists()) {
            m_view->appendLog("Ошибка: выберите модель YOLO (*.pt/*.onnx) из папки yolo_dir.");
            return;
        }

        const QString ydir = yoloDirAbs();
        if (!isPathInsideDirOrUnknown(yolo, ydir)) {
            m_view->appendLog("Ошибка: модель должна быть внутри папки: " + ydir);
            return;
        }


        s.setValue("ui/last_input_path", in);
        s.setValue("ui/last_image_dir", QFileInfo(in).absolutePath());
        s.setValue("ui/last_yolo_model_path", yolo);
        s.sync();

        m_lastRunImagePath = in;
        m_view->clearRunKeepPreview();
        m_runner->runFullDistance(in, out, dev, yolo);
    });

    bindRunner();
}

QString FullDistanceTab::yoloDirAbs() const {
    QFileInfo fi(m_cfg.yoloDir);
    if (fi.isAbsolute()) return QDir::cleanPath(fi.absoluteFilePath());
    return QDir(m_appDir).filePath(m_cfg.yoloDir);
}

void FullDistanceTab::reloadYoloModels() {
    if (!m_yoloModel) return;

    const QString ydir = yoloDirAbs();
    QDir d(ydir);

    QStringList files;
    files << d.entryList(QStringList() << "*.pt" << "*.onnx", QDir::Files, QDir::Name);

    QSignalBlocker b(m_yoloModel);
    m_yoloModel->clear();
    for (const QString& fn : files) {
        const QString abs = d.filePath(fn);
        m_yoloModel->addItem(fn, abs);
    }
}

QString FullDistanceTab::currentYoloModelPath() const {
    if (!m_yoloModel) return {};

    QString t = m_yoloModel->currentText().trimmed();
    if (t.isEmpty()) return {};

    const QString ydir = yoloDirAbs();
    QFileInfo fi(t);

    if (!fi.isAbsolute()) {
        if (!ydir.isEmpty()) {
            const QString abs = QDir(ydir).filePath(t);
            return QDir::cleanPath(abs);
        }
        return {};
    }

    return fi.absoluteFilePath();
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