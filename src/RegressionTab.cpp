#include "RegressionTab.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QProcess>
#include <QDir>
#include <QFileInfo>

RegressionTab::RegressionTab(QWidget* parent) : QWidget(parent) {
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
    m_command = new QLineEdit();
    m_browseCmd = new QPushButton("Команда регрессии");
    row3->addWidget(m_command, 1);
    row3->addWidget(m_browseCmd);

    m_run = new QPushButton("Запуск");
    m_log = new QTextEdit();
    m_log->setReadOnly(true);
    m_log->setMinimumHeight(160);

    root->addLayout(row1);
    root->addLayout(row2);
    root->addLayout(row3);
    root->addWidget(m_run);
    root->addWidget(m_log, 1);

    connect(m_browse, &QPushButton::clicked, this, [this]{
        QString p = pickFile(this, "Выберите изображение", "Images (*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff *.heic *.heif)");
        if (!p.isEmpty()) m_input->setText(p);
    });
    connect(m_browseOut, &QPushButton::clicked, this, [this]{
        QString p = pickDir(this, "Выберите папку вывода");
        if (!p.isEmpty()) m_outputDir->setText(p);
    });
    connect(m_browseCmd, &QPushButton::clicked, this, [this]{
        QString p = pickFile(this, "Выберите .exe/.bat/.cmd/.py", "Any (*.exe *.bat *.cmd *.py);;All (*.*)");
        if (!p.isEmpty()) m_command->setText(p);
    });

    connect(m_run, &QPushButton::clicked, this, [this]{
        const QString in = m_input->text().trimmed();
        const QString out = m_outputDir->text().trimmed();
        const QString cmd = m_command->text().trimmed();

        if (in.isEmpty() || out.isEmpty() || cmd.isEmpty()) {
            m_log->append("Ошибка: задайте фото, папку вывода и команду регрессии.");
            return;
        }
        QDir().mkpath(out);

        auto* p = new QProcess(this);
        connect(p, &QProcess::readyReadStandardOutput, this, [this, p]{
            m_log->append(QString::fromUtf8(p->readAllStandardOutput()));
        });
        connect(p, &QProcess::readyReadStandardError, this, [this, p]{
            m_log->append(QString::fromUtf8(p->readAllStandardError()));
        });
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this, p](int code, QProcess::ExitStatus st){
            m_log->append(QString("Завершено: code=%1 status=%2").arg(code).arg((int)st));
            p->deleteLater();
        });

        QFileInfo fi(cmd);
        if (fi.suffix().toLower() == "py") {
            p->setProgram("python");
            p->setArguments({cmd, "--input", in, "--output-dir", out});
        } else {
            p->setProgram(cmd);
            p->setArguments({"--input", in, "--output-dir", out});
        }
        p->start();
    });
}

QString RegressionTab::pickFile(QWidget* parent, const QString& title, const QString& filter) {
    return QFileDialog::getOpenFileName(parent, title, QString(), filter);
}

QString RegressionTab::pickDir(QWidget* parent, const QString& title) {
    return QFileDialog::getExistingDirectory(parent, title, QString(), QFileDialog::ShowDirsOnly);
}