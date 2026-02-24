#pragma once
#include <QWidget>
#include <QLabel>
#include <QTextEdit>
#include "ModelTypes.h"

class ResultView : public QWidget {
    Q_OBJECT
public:
    explicit ResultView(QWidget* parent = nullptr);

    void clearAll();
    void setBeforeImage(const QString& path);
    void setResult(const ModuleResult& r);

    QTextEdit* logEdit();

private:
    QLabel* m_imgBefore = nullptr; // ДО
    QLabel* m_imgAfter  = nullptr; // ИТОГ
    QTextEdit* m_log = nullptr;

    static void setImageToLabel(QLabel* lbl, const QString& path);
    static QString metaToOneLine(const QJsonObject& o);
};