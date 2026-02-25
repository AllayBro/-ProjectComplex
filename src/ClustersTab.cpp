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

static QString uiIniPathClusters() {
    return QDir(QCoreApplication::applicationDirPath()).filePath("ui.ini");
}

static QString uiDefaultImagesDirClusters() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (d.isEmpty()) d = QDir::homePath();
    return d;
}

static QString uiDefaultOutDirClusters() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (d.isEmpty()) d = QDir::homePath();
    return d;
}

ClustersTab::ClustersTab(const AppConfig& cfg, const QString& appDir, QWidget* parent)
    : QWidget(parent), m_cfg(cfg) {

    m_runner = new RunnerClient(cfg, appDir, this);

    auto* root = new QVBoxLayout(this);

    auto* row1 = new QHBoxLayout();
    m_input = new QLineEdit();
    m_browse = new QPushButton("Выбрать фото");
    row1->addWidget(m_input, 1);
    row1->addWidget(m_browse);

    auto* row2 = new QHBoxLayout();
    m_outputDir = new QLineEdit();
    m_browseOut = new QPushButton("Папка вывода");
    row2->addWidget(m_outputDir, 1);
    row2->addWidget(m_browseOut);

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

    auto* scroll = new QScrollArea();
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
        const QString lastIn  = s.value("ui/last_input_path", "").toString();
        const QString lastOut = s.value("ui/last_output_dir", uiDefaultOutDirClusters()).toString();

        if (!lastOut.isEmpty()) {
            QSignalBlocker b(m_outputDir);
            m_outputDir->setText(lastOut);
        }
        if (!lastIn.isEmpty() && QFileInfo(lastIn).exists()) {
            QSignalBlocker b(m_input);
            m_input->setText(lastIn);
            m_view->setPreviewImage(lastIn);
        }
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
    });

    connect(m_browseOut, &QPushButton::clicked, this, [this] {
        QSettings s(uiIniPathClusters(), QSettings::IniFormat);

        const QString startDir = s.value("ui/last_output_dir", uiDefaultOutDirClusters()).toString();
        const QString p = QFileDialog::getExistingDirectory(this, "Выберите папку вывода", startDir, QFileDialog::ShowDirsOnly);

        if (!p.isEmpty()) {
            s.setValue("ui/last_output_dir", p);
            s.sync();

            QSignalBlocker b(m_outputDir);
            m_outputDir->setText(p);
        }
    });

    for (int i = 0; i < m_clusterButtons.size(); ++i) {
        const int clusterId = m_cfg.clusters[i].clusterId;
        connect(m_clusterButtons[i], &QPushButton::clicked, this, [this, clusterId] {
            const QString in = m_input->text().trimmed();
            const QString out = m_outputDir->text().trimmed();
            const QString dev = m_device->currentText();

            if (in.isEmpty() || out.isEmpty()) {
                m_view->logEdit()->append("Ошибка: задайте фото и папку вывода.");
                return;
            }
            QDir().mkpath(out);

            QSettings s(uiIniPathClusters(), QSettings::IniFormat);
            s.setValue("ui/last_input_path", in);
            s.setValue("ui/last_image_dir", QFileInfo(in).absolutePath());
            s.setValue("ui/last_output_dir", out);
            s.sync();

            m_view->clearRunKeepPreview();
            m_runner->runCluster(clusterId, in, out, dev);
        });
    }

    bindRunner();
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
    });
}