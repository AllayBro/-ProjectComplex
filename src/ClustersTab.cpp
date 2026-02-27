#include "ClustersTab.h"

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

static QString uiIniPathClusters() {
    return QDir(QCoreApplication::applicationDirPath()).filePath("ui.ini");
}

static QString uiDefaultImagesDirClusters() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (d.isEmpty()) d = QDir::homePath();
    return d;
}


ClustersTab::ClustersTab(const AppConfig& cfg, const QString& appDir, QWidget* parent)
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

    auto* rowBtns = new QHBoxLayout();
    for (const auto& c : m_cfg.clusters) {
        auto* b = new QPushButton(QString::number(c.clusterId) + ": " + c.title);
        b->setMinimumHeight(42);
        m_clusterButtons.push_back(b);
        rowBtns->addWidget(b);
    }

    m_view = new ResultView();

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
            m_view->setPreviewImage(lastIn);
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
            m_view->setPreviewImage(p);
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

        m_view->setPreviewImage(fi.absoluteFilePath());
        emit imageSelected(fi.absoluteFilePath());
    });

    connect(m_browseYolo, &QPushButton::clicked, this, [this] {
        const QString ydir = yoloDirAbs();
        if (ydir.isEmpty() || !QDir(ydir).exists()) {
            m_view->logEdit()->append("Ошибка: папка yolo_dir не найдена: " + ydir);
            return;
        }

        const QString p = QFileDialog::getOpenFileName(
            this,
            "Выберите модель YOLO",
            ydir,
            "YOLO weights (*.pt *.onnx)"
        );
        if (p.isEmpty()) return;

        const QString canonDir = QDir(ydir).canonicalPath();
        const QString canonFile = QFileInfo(p).canonicalFilePath();
        if (!canonDir.isEmpty() && !canonFile.isEmpty() && !canonFile.startsWith(canonDir + QDir::separator())) {
            m_view->logEdit()->append("Ошибка: модель должна быть внутри папки: " + ydir);
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

    connect(m_yoloModel, &QComboBox::currentTextChanged, this, [this](const QString& t) {
        const QString p = t.trimmed();
        if (p.isEmpty()) return;
        QSettings s(uiIniPathClusters(), QSettings::IniFormat);
        s.setValue("ui/last_yolo_model_path", p);
        s.sync();
    });

    for (int i = 0; i < m_clusterButtons.size(); ++i) {
        const int clusterId = m_cfg.clusters[i].clusterId;
        connect(m_clusterButtons[i], &QPushButton::clicked, this, [this, clusterId] {
            const QString in = m_input->text().trimmed();
            const QString dev = m_device->currentText();

            const QString out = QDir(QDir(m_appDir).filePath("run"))
                    .filePath(QString("cluster_%1").arg(clusterId));

            const QString yolo = currentYoloModelPath();
            if (in.isEmpty()) {
                m_view->logEdit()->append("Ошибка: задайте фото.");
                return;
            }
            if (yolo.isEmpty() || !QFileInfo(yolo).exists()) {
                m_view->logEdit()->append("Ошибка: выберите модель YOLO (*.pt/*.onnx) из папки yolo_dir.");
                return;
            }

            const QString ydir = yoloDirAbs();
            const QString canonDir = QDir(ydir).canonicalPath();
            const QString canonFile = QFileInfo(yolo).canonicalFilePath();
            if (!canonDir.isEmpty() && !canonFile.isEmpty() && !canonFile.startsWith(canonDir + QDir::separator())) {
                m_view->logEdit()->append("Ошибка: модель должна быть внутри папки: " + ydir);
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
    QFileInfo fi(m_cfg.yoloDir);
    if (fi.isAbsolute()) return QDir::cleanPath(fi.absoluteFilePath());
    return QDir(m_appDir).filePath(m_cfg.yoloDir);
}

void ClustersTab::reloadYoloModels() {
    if (!m_yoloModel) return;

    const QString ydir = yoloDirAbs();
    QDir d(ydir);

    QStringList files;
    files << d.entryList(QStringList() << "*.pt" << "*.onnx", QDir::Files, QDir::Name);

    QSignalBlocker b(m_yoloModel);
    m_yoloModel->clear();
    for (const QString& fn : files) {
        m_yoloModel->addItem(fn, d.filePath(fn));
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

void ClustersTab::bindRunner() {
    connect(m_runner, &RunnerClient::started, this, [this] {
        m_view->logEdit()->append("Запуск...");
    });

    connect(m_runner, &RunnerClient::logLine, this, [this](const QString& s) {
        m_view->logEdit()->append(s);
    });

    connect(m_runner, &RunnerClient::finishedError, this, [this](const QString& e) {
        m_view->logEdit()->append("Ошибка: " + e);
    });

    connect(m_runner, &RunnerClient::finishedOk, this, [this](const ModuleResult& r) {
        m_view->setResult(r);
        emit resultReady(m_lastRunImagePath, r);
    });
}