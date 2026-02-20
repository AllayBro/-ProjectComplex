#pragma once
#include <QWidget>
#include <QLabel>
#include <QTableWidget>
#include <QTextEdit>
#include "ModelTypes.h"

class ResultView : public QWidget {
    Q_OBJECT
public:
    explicit ResultView(QWidget* parent = nullptr);

    void clearAll();
    void setResult(const ModuleResult& r);

    QTextEdit* logEdit();

private:
    QLabel* m_imgAnnotated = nullptr;
    QLabel* m_imgCleaned = nullptr;
    QTableWidget* m_table = nullptr;
    QTextEdit* m_log = nullptr;

    static void setImageToLabel(QLabel* lbl, const QString& path);
    static QString metaToOneLine(const QJsonObject& o);
};