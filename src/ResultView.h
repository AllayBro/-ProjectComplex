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
#include <QJsonObject>
#include <QJsonArray>
#include <QVector>
#include <QImage>

#include "ModelTypes.h"

class QScrollArea;

class ResultView : public QWidget {
    Q_OBJECT
public:
    explicit ResultView(QWidget* parent = nullptr);

    void setPreviewFromRaw(const QString& originalPath, const QImage& preview, const QJsonObject& exifRoot);

    QTextEdit* logEdit();
    void appendLog(const QString& s);

    void clearAll();
    void clearRunKeepPreview();

    void setPreviewImage(const QString& originalPath);
    void setResult(const ModuleResult& r);

    void setPreviewFromRunner(const QString& originalPath, const QString& displayPath, const QJsonObject& exifRoot);
    void setPythonExe(const QString& pythonExe);

private slots:
    void onSaveImage();
    void onExportData();

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    QLabel* m_imgOriginal = nullptr;   // слева: исходник
    QLabel* m_imgResult = nullptr;     // справа: результат

    QTabWidget* m_tabs = nullptr;      // Консоль / Таблицы / Графики
    QTextEdit*  m_log  = nullptr;

    QTabWidget*  m_tables = nullptr;   // подвкладки таблиц
    QTableWidget* m_tblDetections = nullptr;
    QTableWidget* m_tblExif = nullptr;

    QTabWidget*  m_plots = nullptr;    // подвкладки графиков

    QPushButton* m_btnSaveImage = nullptr;
    QPushButton* m_btnExportData = nullptr;

    QString m_originalPath;
    QString m_pythonExe = "python";
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

    struct PlotCache {
        QScrollArea* area = nullptr;
        QLabel* label = nullptr;
        QPixmap src;
        quint64 key = 0;
        QSize target;
        QPixmap scaled;
    };
    QVector<PlotCache> m_plotCaches;

    void applyScaled(QLabel* lbl,
                     const QPixmap& src,
                     quint64& lastKey,
                     QSize& lastTarget,
                     QPixmap& cachedScaled);

    void applyScaledToTarget(QLabel* lbl,
                             const QPixmap& src,
                             quint64& lastKey,
                             QSize& lastTarget,
                             QPixmap& cachedScaled,
                             const QSize& target);

    void loadOriginal(const QString& path);
    void renderResultPixmap();         // рисуем результат в Qt

    void rebuildDetectionsTable();     // detections → m_tblDetections
    void rebuildExifTable();           // exif → m_tblExif
    void rebuildExtraTablesTabs();     // tables[] → подвкладки
    void rebuildPlotsTabs();           // plots[] → подвкладки

    void clearDynamicTableTabs();      // удалить подвкладки таблиц (кроме базовых)
    void clearPlotTabs();              // удалить все вкладки графиков

    void rescaleAllPlots();

    QStringList buildColumns(QStringList& metaKeysOut) const;
    static QTableWidget* makeStdTable(QWidget* parent = nullptr);
    static QString jsonValueToText(const QJsonValue& v);
    static QString csvEscape(const QString& s);

    static bool loadCsvToTable(const QString& path, QTableWidget* t, QString& err);
    static QWidget* buildTableWidgetFromEntry(const QJsonObject& entry, QString& outTitle, QString& err);
    static QTableWidget* buildKvTableFromObject(const QJsonObject& obj, QWidget* parent = nullptr);
    static QTableWidget* buildInlineTableFromEntry(const QJsonObject& entry, QWidget* parent = nullptr);

    bool exportCSV(const QString& path, QString& err) const;
    bool exportJSON(const QString& path, QString& err) const;
    bool exportTXT(const QString& path, QString& err) const;
    bool exportHTML(const QString& path, QString& err) const;
    bool exportSQLite(const QString& path, QString& err) const;
    bool exportXLSX(const QString& path, QString& err) const; // требует QXlsx
};