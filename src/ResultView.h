#pragma once
#include <QWidget>
#include <QLabel>
#include <QTableWidget>
#include <QTextEdit>
#include <QTabWidget>
#include <QPushButton>
#include <QPixmap>
#include <QResizeEvent>
#include <QSize>
#include <QJsonValue>

#include "ModelTypes.h"

class ResultView : public QWidget {
    Q_OBJECT
public:
    explicit ResultView(QWidget* parent = nullptr);
    QTextEdit* logEdit();
    void appendLog(const QString& s);
    void clearAll();
    void clearRunKeepPreview();

    void setPreviewImage(const QString& originalPath);
    void setResult(const ModuleResult& r);

private slots:
    void onSaveImage();
    void onExportData();

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    QLabel* m_imgOriginal = nullptr;   // слева: исходник
    QLabel* m_imgResult = nullptr;     // справа: результат

    QTabWidget* m_tabs = nullptr;
    QTableWidget* m_table = nullptr;
    QTextEdit* m_log = nullptr;

    QPushButton* m_btnSaveImage = nullptr;
    QPushButton* m_btnExportData = nullptr;

    QString m_originalPath;
    ModuleResult m_lastResult;
    bool m_hasResult = false;

    QPixmap m_srcOriginal;
    QPixmap m_srcResult;

    quint64 m_keyOriginal = 0;
    quint64 m_keyResult = 0;
    QSize   m_targetOriginal;
    QSize   m_targetResult;
    QPixmap m_scaledOriginal;
    QPixmap m_scaledResult;

    void applyScaled(QLabel* lbl,
                     const QPixmap& src,
                     quint64& lastKey,
                     QSize& lastTarget,
                     QPixmap& cachedScaled);

    void loadOriginal(const QString& path);
    void renderResultPixmap();         // рисуем результат в Qt

    void rebuildTable();               // динамические колонки: только из meta
    QStringList buildColumns(QStringList& metaKeysOut) const;

    static QString jsonValueToText(const QJsonValue& v);
    static QString csvEscape(const QString& s);

    bool exportCSV(const QString& path, QString& err) const;
    bool exportJSON(const QString& path, QString& err) const;
    bool exportTXT(const QString& path, QString& err) const;
    bool exportHTML(const QString& path, QString& err) const;
    bool exportSQLite(const QString& path, QString& err) const;

    bool exportXLSX(const QString& path, QString& err) const; // требует QXlsx
};