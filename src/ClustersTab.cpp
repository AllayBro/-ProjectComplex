#include "ClustersTab.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDir>
#include <QLabel>

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

    root->addLayout(row1);
    root->addLayout(row2);
    root->addLayout(row3);
    root->addLayout(rowBtns);
    root->addWidget(m_view, 1);

    connect(m_browse, &QPushButton::clicked, this, [=]{
        const QString p = QFileDialog::getOpenFileName(this, "Выберите изображение", QString(),
            "Images (*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff *.heic *.heif)");
        if (!p.isEmpty()) m_input->setText(p);
    });

    connect(m_browseOut, &QPushButton::clicked, this, [=]{
        const QString p = QFileDialog::getExistingDirectory(this, "Выберите папку вывода", QString(), QFileDialog::ShowDirsOnly);
        if (!p.isEmpty()) m_outputDir->setText(p);
    });

    for (int i = 0; i < m_clusterButtons.size(); ++i) {
        const int clusterId = m_cfg.clusters[i].clusterId;
        connect(m_clusterButtons[i], &QPushButton::clicked, this, [=]{
            const QString in = m_input->text().trimmed();
            const QString out = m_outputDir->text().trimmed();
            const QString dev = m_device->currentText();

            if (in.isEmpty() || out.isEmpty()) {
                m_view->logEdit()->append("Ошибка: задайте фото и папку вывода.");
                return;
            }
            QDir().mkpath(out);

            m_view->clearAll();
            m_runner->runCluster(clusterId, in, out, dev);
        });
    }

    bindRunner();
}

void ClustersTab::bindRunner() {
    connect(m_runner, &RunnerClient::started, this, [=]{
        m_view->logEdit()->append("Запуск...");
    });
    connect(m_runner, &RunnerClient::logLine, this, [=](const QString& s){
        m_view->logEdit()->append(s);
    });
    connect(m_runner, &RunnerClient::finishedError, this, [=](const QString& e){
        m_view->logEdit()->append("Ошибка: " + e);
    });
    connect(m_runner, &RunnerClient::finishedOk, this, [=](const ModuleResult& r){
        m_view->setResult(r);
    });
}