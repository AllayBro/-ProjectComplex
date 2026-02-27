#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>


class RegressionTab : public QWidget {
    Q_OBJECT
public:
    explicit RegressionTab(QWidget* parent = nullptr);

private:
    QLineEdit* m_input = nullptr;
    QPushButton* m_browse = nullptr;

    QLineEdit* m_outputDir = nullptr;
    QPushButton* m_browseOut = nullptr;

    QLineEdit* m_command = nullptr;
    QPushButton* m_browseCmd = nullptr;

    QPushButton* m_run = nullptr;
    QTextEdit* m_log = nullptr;

    static QString pickFile(QWidget* parent, const QString& title, const QString& filter);
    static QString pickDir(QWidget* parent, const QString& title);
};