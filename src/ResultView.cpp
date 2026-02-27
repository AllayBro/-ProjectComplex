#include "ResultView.h"

#include <QSettings>
#include <QCoreApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileInfo>
#include <QFileDialog>
#include <QDateTime>
#include <QJsonDocument>
#include <QPainter>
#include <QPen>
#include <QFont>
#include <QBuffer>
#include <QByteArray>
#include <QSet>
#include <QDir>
#include <QScrollArea>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>

#ifdef VK_WITH_QXLSX
#include <QXlsx/xlsxdocument.h>
#include <QXlsx/xlsxformat.h>
#endif
ResultView::ResultView(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* topBar = new QHBoxLayout();
    m_btnSaveImage = new QPushButton("Сохранить изображение...");
    m_btnExportData = new QPushButton("Экспорт данных...");
    topBar->addStretch(1);
    topBar->addWidget(m_btnSaveImage);
    topBar->addWidget(m_btnExportData);

    auto* imgs = new QHBoxLayout();
    m_imgOriginal = new QLabel();
    m_imgOriginal->setAlignment(Qt::AlignCenter);
    m_imgOriginal->setFrameShape(QFrame::Box);
    m_imgOriginal->setMinimumHeight(260);
    m_imgOriginal->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_imgResult = new QLabel();
    m_imgResult->setAlignment(Qt::AlignCenter);
    m_imgResult->setFrameShape(QFrame::Box);
    m_imgResult->setMinimumHeight(260);
    m_imgResult->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    imgs->addWidget(m_imgOriginal, 1);
    imgs->addWidget(m_imgResult, 1);

    m_tabs = new QTabWidget();

    // Консоль
    m_log = new QTextEdit();
    m_log->setReadOnly(true);
    m_log->setLineWrapMode(QTextEdit::NoWrap);
    m_log->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_log->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Таблицы (подвкладки)
    m_tables = new QTabWidget();
    m_tblDetections = makeStdTable(m_tables);
    m_tblExif = makeStdTable(m_tables);
    m_tblExif->setSortingEnabled(false);
    m_tblExif->setColumnCount(2);
    m_tblExif->setHorizontalHeaderLabels(QStringList() << "key" << "value");
    m_tblExif->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tblExif->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    m_tables->addTab(m_tblDetections, "Detections");
    m_tables->addTab(m_tblExif, "EXIF");

    // Графики (подвкладки)
    m_plots = new QTabWidget();
    connect(m_plots, &QTabWidget::currentChanged, this, [this](int){ rescaleAllPlots(); });

    m_tabs->addTab(m_log, "Консоль");
    m_tabs->addTab(m_tables, "Таблицы");
    m_tabs->addTab(m_plots, "Графики");
    m_tabs->setCurrentWidget(m_log);

    root->addLayout(topBar);
    root->addLayout(imgs, 2);
    root->addWidget(m_tabs, 3);

    connect(m_btnSaveImage, &QPushButton::clicked, this, &ResultView::onSaveImage);
    connect(m_btnExportData, &QPushButton::clicked, this, &ResultView::onExportData);
}

void ResultView::applyScaled(QLabel* lbl,
                             const QPixmap& src,
                             quint64& lastKey,
                             QSize& lastTarget,
                             QPixmap& cachedScaled) {
    if (!lbl) return;
    if (src.isNull()) {
        lbl->clear();
        lastKey = 0;
        lastTarget = QSize();
        cachedScaled = QPixmap();
        return;
    }

    const QSize target = lbl->size();
    const quint64 key = src.cacheKey();

    if (key == lastKey && target == lastTarget && !cachedScaled.isNull()) {
        lbl->setPixmap(cachedScaled);
        return;
    }

    lastKey = key;
    lastTarget = target;

    if (target.width() <= 0 || target.height() <= 0) {
        cachedScaled = src;
        lbl->setPixmap(src);
        return;
    }

    cachedScaled = src.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    lbl->setPixmap(cachedScaled);
}

void ResultView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    applyScaled(m_imgOriginal, m_srcOriginal, m_keyOriginal, m_targetOriginal, m_scaledOriginal);
    applyScaled(m_imgResult,   m_srcResult,   m_keyResult,   m_targetResult,   m_scaledResult);
    rescaleAllPlots();
}

void ResultView::clearAll() {
    m_originalPath.clear();
    m_hasResult = false;
    m_lastResult = ModuleResult();

    m_srcOriginal = QPixmap();
    m_srcResult = QPixmap();

    applyScaled(m_imgOriginal, m_srcOriginal, m_keyOriginal, m_targetOriginal, m_scaledOriginal);
    applyScaled(m_imgResult,   m_srcResult,   m_keyResult,   m_targetResult,   m_scaledResult);

    clearDynamicTableTabs();
    if (m_tblDetections) { m_tblDetections->setRowCount(0); m_tblDetections->setColumnCount(0); }
    if (m_tblExif) { m_tblExif->setRowCount(0); m_tblExif->setColumnCount(2); }

    clearPlotTabs();

    if (m_log) m_log->clear();
    if (m_tabs) m_tabs->setCurrentWidget(m_log);
}

void ResultView::clearRunKeepPreview() {
    m_hasResult = false;
    m_lastResult = ModuleResult();

    m_srcResult = QPixmap();
    applyScaled(m_imgResult, m_srcResult, m_keyResult, m_targetResult, m_scaledResult);

    clearDynamicTableTabs();
    if (m_tblDetections) { m_tblDetections->setRowCount(0); m_tblDetections->setColumnCount(0); }
    if (m_tblExif) { m_tblExif->setRowCount(0); m_tblExif->setColumnCount(2); }

    clearPlotTabs();

    if (m_log) m_log->clear();
    if (m_tabs) m_tabs->setCurrentWidget(m_log);
}

void ResultView::loadOriginal(const QString& path) {
    m_srcOriginal = QPixmap();
    if (path.isEmpty()) return;

    QFileInfo fi(path);
    if (!fi.exists()) return;

    QPixmap pm(path);
    if (pm.isNull()) return;

    m_srcOriginal = pm;
}

void ResultView::setPreviewImage(const QString& originalPath) {
    m_originalPath = originalPath;
    loadOriginal(originalPath);

    applyScaled(m_imgOriginal, m_srcOriginal, m_keyOriginal, m_targetOriginal, m_scaledOriginal);

    // справа пока пусто (результат появится после обработки)
    m_srcResult = QPixmap();
    applyScaled(m_imgResult, m_srcResult, m_keyResult, m_targetResult, m_scaledResult);

    clearDynamicTableTabs();
    if (m_tblDetections) { m_tblDetections->setRowCount(0); m_tblDetections->setColumnCount(0); }
    if (m_tblExif) { m_tblExif->setRowCount(0); m_tblExif->setColumnCount(2); }

    clearPlotTabs();

    m_log->clear();
    if (m_tabs) m_tabs->setCurrentWidget(m_log);
}

QString ResultView::jsonValueToText(const QJsonValue& v) {
    if (v.isNull() || v.isUndefined()) return {};
    if (v.isString()) return v.toString();
    if (v.isBool()) return v.toBool() ? "true" : "false";
    if (v.isDouble()) return QString::number(v.toDouble(), 'f', 6);
    if (v.isArray()) {
        QJsonDocument d(v.toArray());
        return QString::fromUtf8(d.toJson(QJsonDocument::Compact));
    }
    if (v.isObject()) {
        QJsonDocument d(v.toObject());
        return QString::fromUtf8(d.toJson(QJsonDocument::Compact));
    }
    return {};
}

QString ResultView::csvEscape(const QString& s) {
    QString out = s;
    if (out.contains('\"')) out.replace('\"', "\"\"");
    const bool needQuotes = out.contains(',') || out.contains('\n') || out.contains('\r') || out.contains('\"');
    if (needQuotes) out = '\"' + out + '\"';
    return out;
}

QStringList ResultView::buildColumns(QStringList& metaKeysOut) const {
    QSet<QString> keys;
    keys.reserve(64);

    for (const auto& d : m_lastResult.detections) {
        for (auto it = d.meta.begin(); it != d.meta.end(); ++it) {
            const QString k = it.key();
            if (k == "id" || k == "cls" || k == "conf" || k == "w_px" || k == "h_px") continue;
            keys.insert(k);
        }
    }

    metaKeysOut = keys.values();
    std::sort(metaKeysOut.begin(), metaKeysOut.end());

    QStringList cols;
    cols << "id" << "cls" << "conf" << "w_px" << "h_px";
    cols << metaKeysOut;
    return cols;
}

void ResultView::rebuildDetectionsTable() {
    if (!m_tblDetections) return;

    QStringList metaKeys;
    const QStringList cols = buildColumns(metaKeys);

    m_tblDetections->setSortingEnabled(false);
    m_tblDetections->clear();
    m_tblDetections->setColumnCount(cols.size());
    m_tblDetections->setHorizontalHeaderLabels(cols);
    m_tblDetections->setRowCount(m_lastResult.detections.size());

    for (int i = 0; i < m_lastResult.detections.size(); ++i) {
        const auto& d = m_lastResult.detections[i];
        const int w = d.x2 - d.x1;
        const int h = d.y2 - d.y1;

        m_tblDetections->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
        m_tblDetections->setItem(i, 1, new QTableWidgetItem(d.clsName));
        m_tblDetections->setItem(i, 2, new QTableWidgetItem(QString::number(d.conf, 'f', 4)));
        m_tblDetections->setItem(i, 3, new QTableWidgetItem(QString::number(w)));
        m_tblDetections->setItem(i, 4, new QTableWidgetItem(QString::number(h)));

        for (int c = 0; c < metaKeys.size(); ++c) {
            const QString& k = metaKeys[c];
            const QJsonValue v = d.meta.value(k);
            if (!v.isUndefined() && !v.isNull()) {
                m_tblDetections->setItem(i, 5 + c, new QTableWidgetItem(jsonValueToText(v)));
            }
        }
    }

    m_tblDetections->setSortingEnabled(true);
}

void ResultView::renderResultPixmap() {
    m_srcResult = QPixmap();
    if (m_srcOriginal.isNull()) return;
    if (!m_hasResult) return;

    QImage img = m_srcOriginal.toImage().convertToFormat(QImage::Format_ARGB32);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    QPen pen(QColor(0, 255, 0));
    pen.setWidth(2);
    p.setPen(pen);

    QFont f = p.font();
    f.setPointSize(10);
    f.setBold(true);
    p.setFont(f);

    for (int i = 0; i < m_lastResult.detections.size(); ++i) {
        const auto& d = m_lastResult.detections[i];

        QRect r(d.x1, d.y1, d.x2 - d.x1, d.y2 - d.y1);
        p.drawRect(r);

        QString label = QString("%1 %2").arg(d.clsName).arg(i + 1);

        // если есть dist_m — добавим
        if (d.meta.contains("dist_m")) {
            label += " ~" + QString::number(d.meta.value("dist_m").toDouble(), 'f', 2) + "m";
        }

        const int tx = r.left();
        const int ty = std::max(12, r.top() - 4);
        p.drawText(tx, ty, label);
    }

    p.end();
    m_srcResult = QPixmap::fromImage(img);
}

void ResultView::setResult(const ModuleResult& r) {
    // слева НЕ трогаем: там всегда исходник
    m_lastResult = r;
    m_hasResult = true;

    rebuildDetectionsTable();
    renderResultPixmap();
    applyScaled(m_imgResult, m_srcResult, m_keyResult, m_targetResult, m_scaledResult);

    rebuildExifTable();
    rebuildExtraTablesTabs();
    rebuildPlotsTabs();

    // Консоль
    if (m_log) {
        const QString cur = m_log->toPlainText().trimmed();

        if (cur.isEmpty()) {
            if (!r.consoleStdout.isEmpty() || !r.consoleStderr.isEmpty()) {
                QStringList all;
                all << r.consoleStdout;
                if (!r.consoleStderr.isEmpty()) {
                    all << "---- STDERR ----";
                    all << r.consoleStderr;
                }
                m_log->setPlainText(all.join("\n"));
            }
        }

        m_log->append("");
        m_log->append("---- RESULT ----");
        m_log->append("module_id=" + r.moduleId);
        m_log->append("device=" + r.deviceUsed);
        m_log->append("image=" + QString::number(r.imageW) + "x" + QString::number(r.imageH));

        if (!r.timingsMs.isEmpty()) {
            QJsonDocument td(r.timingsMs);
            m_log->append("timings_ms=" + QString::fromUtf8(td.toJson(QJsonDocument::Compact)));
        }
        if (!r.warnings.isEmpty()) {
            m_log->append("warnings=" + r.warnings.join(" | "));
        }
        m_log->append("detections=" + QString::number(r.detections.size()));
    }
}

QTableWidget* ResultView::makeStdTable(QWidget* parent) {
    auto* t = new QTableWidget(parent);
    t->setColumnCount(0);
    t->setRowCount(0);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::ExtendedSelection);
    t->setSelectionBehavior(QAbstractItemView::SelectItems);
    t->setSortingEnabled(true);
    t->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    t->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    t->horizontalHeader()->setStretchLastSection(false);
    return t;
}

void ResultView::applyScaledToTarget(QLabel* lbl,
                                    const QPixmap& src,
                                    quint64& lastKey,
                                    QSize& lastTarget,
                                    QPixmap& cachedScaled,
                                    const QSize& target) {
    if (!lbl) return;
    if (src.isNull()) {
        lbl->clear();
        lastKey = 0;
        lastTarget = QSize();
        cachedScaled = QPixmap();
        return;
    }

    const quint64 key = src.cacheKey();
    if (key == lastKey && target == lastTarget && !cachedScaled.isNull()) {
        lbl->setPixmap(cachedScaled);
        return;
    }

    lastKey = key;
    lastTarget = target;

    if (target.width() <= 0 || target.height() <= 0) {
        cachedScaled = src;
        lbl->setPixmap(src);
        return;
    }

    cachedScaled = src.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    lbl->setPixmap(cachedScaled);
}

static QStringList parseCsvLine(const QString& line) {
    QStringList out;
    QString cur;
    bool inq = false;

    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line[i];

        if (inq) {
            if (ch == QLatin1Char('"')) {
                if (i + 1 < line.size() && line[i + 1] == QLatin1Char('"')) {
                    cur += QLatin1Char('"');
                    ++i;
                } else {
                    inq = false;
                }
            } else {
                cur += ch;
            }
        } else {
            if (ch == QLatin1Char(',')) {
                out << cur;
                cur.clear();
            } else if (ch == QLatin1Char('"')) {
                inq = true;
            } else {
                cur += ch;
            }
        }
    }
    out << cur;
    return out;
}

bool ResultView::loadCsvToTable(const QString& path, QTableWidget* t, QString& err) {
    if (!t) { err = "Null table"; return false; }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { err = "Cannot open csv: " + path; return false; }

    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);

    QString headerLine;
    if (!in.atEnd()) headerLine = in.readLine();
    if (headerLine.isEmpty()) { err = "Empty csv: " + path; return false; }

    const QStringList headers = parseCsvLine(headerLine);

    QVector<QStringList> rows;
    rows.reserve(512);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;
        rows.push_back(parseCsvLine(line));
    }

    t->setSortingEnabled(false);
    t->clear();
    t->setColumnCount(headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->setRowCount(rows.size());

    for (int r = 0; r < rows.size(); ++r) {
        const QStringList& row = rows[r];
        for (int c = 0; c < headers.size(); ++c) {
            const QString v = (c < row.size()) ? row[c] : QString();
            t->setItem(r, c, new QTableWidgetItem(v));
        }
    }

    t->setSortingEnabled(true);
    return true;
}

QTableWidget* ResultView::buildKvTableFromObject(const QJsonObject& obj, QWidget* parent) {
    auto* t = makeStdTable(parent);
    t->setSortingEnabled(false);
    t->setColumnCount(2);
    t->setHorizontalHeaderLabels(QStringList() << "key" << "value");
    t->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    t->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    QStringList keys = obj.keys();
    std::sort(keys.begin(), keys.end());

    t->setRowCount(keys.size());
    for (int i = 0; i < keys.size(); ++i) {
        const QString& k = keys[i];
        t->setItem(i, 0, new QTableWidgetItem(k));
        t->setItem(i, 1, new QTableWidgetItem(jsonValueToText(obj.value(k))));
    }

    t->setSortingEnabled(true);
    return t;
}

QTableWidget* ResultView::buildInlineTableFromEntry(const QJsonObject& entry, QWidget* parent) {
    const QJsonArray colsA = entry.value("columns").toArray();
    const QJsonArray rowsA = entry.value("rows").toArray();

    QStringList headers;
    headers.reserve(colsA.size());
    for (const auto& v : colsA) headers << v.toString();

    auto* t = makeStdTable(parent);
    t->setSortingEnabled(false);
    t->clear();
    t->setColumnCount(headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->setRowCount(rowsA.size());

    for (int r = 0; r < rowsA.size(); ++r) {
        const QJsonArray row = rowsA[r].toArray();
        for (int c = 0; c < headers.size(); ++c) {
            const QString v = (c < row.size()) ? jsonValueToText(row[c]) : QString();
            t->setItem(r, c, new QTableWidgetItem(v));
        }
    }

    t->setSortingEnabled(true);
    return t;
}

QWidget* ResultView::buildTableWidgetFromEntry(const QJsonObject& entry, QString& outTitle, QString& err) {
    outTitle = entry.value("title").toString(entry.value("name").toString("Table"));
    const QString type = entry.value("type").toString();

    if (type == "inline") {
        return buildInlineTableFromEntry(entry);
    }
    if (type == "kv") {
        const QJsonObject data = entry.value("data").toObject();
        return buildKvTableFromObject(data);
    }
    if (type == "exif") {
        QJsonObject merged = entry.value("data").toObject();
        const QJsonObject gps = entry.value("gps").toObject();
        if (!gps.isEmpty()) {
            for (auto it = gps.begin(); it != gps.end(); ++it) {
                merged.insert("gps." + it.key(), it.value());
            }
        }
        return buildKvTableFromObject(merged);
    }
    if (type == "csv") {
        const QString p = entry.value("path").toString();
        auto* t = makeStdTable();
        QString e;
        if (!loadCsvToTable(p, t, e)) {
            err = e;
            t->deleteLater();
            return nullptr;
        }
        return t;
    }
    if (type == "json") {
        const QString p = entry.value("path").toString();
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { err = "Cannot open json: " + p; return nullptr; }
        QJsonParseError pe;
        QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
        if (pe.error != QJsonParseError::NoError) { err = "JSON parse error: " + pe.errorString(); return nullptr; }

        if (d.isObject()) {
            return buildKvTableFromObject(d.object());
        }
        if (d.isArray()) {
            const QJsonArray a = d.array();
            if (a.isEmpty()) return makeStdTable();

            QSet<QString> keysSet;
            for (const auto& v : a) {
                const QJsonObject o = v.toObject();
                for (auto it = o.begin(); it != o.end(); ++it) keysSet.insert(it.key());
            }
            QStringList keys = keysSet.values();
            std::sort(keys.begin(), keys.end());

            auto* t = makeStdTable();
            t->setSortingEnabled(false);
            t->clear();
            t->setColumnCount(keys.size());
            t->setHorizontalHeaderLabels(keys);
            t->setRowCount(a.size());

            for (int r = 0; r < a.size(); ++r) {
                const QJsonObject o = a[r].toObject();
                for (int c = 0; c < keys.size(); ++c) {
                    const QString& k = keys[c];
                    t->setItem(r, c, new QTableWidgetItem(jsonValueToText(o.value(k))));
                }
            }
            t->setSortingEnabled(true);
            return t;
        }

        err = "Unsupported json root";
        return nullptr;
    }

    err = "Unknown table type: " + type;
    return nullptr;
}


void ResultView::clearDynamicTableTabs() {
    if (!m_tables) return;
    while (m_tables->count() > 2) {
        QWidget* w = m_tables->widget(2);
        m_tables->removeTab(2);
        if (w) w->deleteLater();
    }
}

void ResultView::clearPlotTabs() {
    if (!m_plots) return;
    while (m_plots->count() > 0) {
        QWidget* w = m_plots->widget(0);
        m_plots->removeTab(0);
        if (w) w->deleteLater();
    }
    m_plotCaches.clear();
}

void ResultView::rebuildExifTable() {
    if (!m_tblExif) return;

    m_tblExif->setSortingEnabled(false);
    m_tblExif->clear();
    m_tblExif->setColumnCount(2);
    m_tblExif->setHorizontalHeaderLabels(QStringList() << "key" << "value");
    m_tblExif->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tblExif->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);

    QJsonObject data;
    QJsonObject gps;

    {
        QJsonObject exRoot = m_lastResult.exif;
        if (exRoot.contains("data") && exRoot.value("data").isObject()) data = exRoot.value("data").toObject();
        else data = exRoot;

        if (exRoot.contains("gps") && exRoot.value("gps").isObject()) gps = exRoot.value("gps").toObject();
    }

    if (data.isEmpty() && gps.isEmpty()) {
        const QJsonArray a = m_lastResult.tables;
        for (const auto& v : a) {
            if (!v.isObject()) continue;
            const QJsonObject e = v.toObject();

            const QString type = e.value("type").toString().trimmed();
            const QString name = e.value("name").toString().trimmed();
            const QString title = e.value("title").toString().trimmed();

            const QString nameLow = name.toLower();
            const QString titleLow = title.toLower();

            const bool looksExif = (nameLow == "exif" || titleLow == "exif" || titleLow == "exif table");

            if (type == "exif") {
                data = e.value("data").toObject();
                gps  = e.value("gps").toObject();
                break;
            }
            if (type == "kv" && looksExif) {
                data = e.value("data").toObject();
                break;
            }
        }
    }

    QJsonObject merged = data;
    if (!gps.isEmpty()) {
        for (auto it = gps.begin(); it != gps.end(); ++it) {
            merged.insert("gps." + it.key(), it.value());
        }
    }

    if (merged.isEmpty()) {
        m_tblExif->setRowCount(1);
        m_tblExif->setItem(0, 0, new QTableWidgetItem("exif"));
        m_tblExif->setItem(0, 1, new QTableWidgetItem("нет данных"));
        m_tblExif->setSortingEnabled(true);
        return;
    }

    QStringList keys = merged.keys();
    std::sort(keys.begin(), keys.end());

    m_tblExif->setRowCount(keys.size());
    for (int i = 0; i < keys.size(); ++i) {
        const QString& k = keys[i];
        m_tblExif->setItem(i, 0, new QTableWidgetItem(k));
        m_tblExif->setItem(i, 1, new QTableWidgetItem(jsonValueToText(merged.value(k))));
    }

    m_tblExif->setSortingEnabled(true);
}
void ResultView::rebuildExtraTablesTabs() {
    clearDynamicTableTabs();
    if (!m_tables) return;

    const QJsonArray a = m_lastResult.tables;
    for (const auto& v : a) {
        if (!v.isObject()) continue;
        const QJsonObject e = v.toObject();

        const QString type = e.value("type").toString().trimmed();
        const QString name = e.value("name").toString().trimmed();
        const QString titleIn = e.value("title").toString().trimmed();

        const QString nameLow = name.toLower();
        const QString titleLow = titleIn.toLower();

        const bool looksExif = (type == "exif") || (nameLow == "exif") || (titleLow == "exif") || (titleLow == "exif table");
        if (looksExif) continue;

        QString title;
        QString err;
        QWidget* w = buildTableWidgetFromEntry(e, title, err);
        if (!w) {
            if (m_log) m_log->append("TABLE SKIP: " + title + " (" + err + ")");
            continue;
        }

        m_tables->addTab(w, title);
    }
}

void ResultView::rebuildPlotsTabs() {
    clearPlotTabs();
    if (!m_plots) return;

    const QJsonArray a = m_lastResult.plots;
    for (const auto& v : a) {
        QString title;
        QString path;

        if (v.isString()) {
            path = v.toString();
            title = QFileInfo(path).completeBaseName();
        } else if (v.isObject()) {
            const QJsonObject o = v.toObject();
            path = o.value("path").toString();
            title = o.value("title").toString(o.value("name").toString(QFileInfo(path).completeBaseName()));
        } else {
            continue;
        }

        if (path.isEmpty() || !QFileInfo(path).exists()) continue;

        QPixmap pm(path);
        if (pm.isNull()) continue;

        auto* lbl = new QLabel();
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

        auto* sa = new QScrollArea();
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setWidget(lbl);

        m_plots->addTab(sa, title);

        PlotCache pc;
        pc.area = sa;
        pc.label = lbl;
        pc.src = pm;
        m_plotCaches.push_back(pc);
    }

    rescaleAllPlots();
}

void ResultView::rescaleAllPlots() {
    for (int i = 0; i < m_plotCaches.size(); ++i) {
        PlotCache& pc = m_plotCaches[i];
        if (!pc.label || pc.src.isNull()) continue;

        QSize target = pc.label->size();
        if (pc.area && pc.area->viewport()) target = pc.area->viewport()->size();

        applyScaledToTarget(pc.label, pc.src, pc.key, pc.target, pc.scaled, target);
    }
}

void ResultView::onSaveImage() {
    if (m_srcResult.isNull()) {
        if (m_log) m_log->append("\nНет результата для сохранения.");
        return;
    }

    const QString filter =
        "PNG (*.png);;"
        "JPG (*.jpg *.jpeg);;"
        "BMP (*.bmp);;"
        "WEBP (*.webp)";

    const QString iniPath = QDir(QCoreApplication::applicationDirPath()).filePath("ui.ini");
    QSettings st(iniPath, QSettings::IniFormat);

    QString suggestedDir = st.value("paths/last_save_image_dir", "").toString();
    if (suggestedDir.isEmpty() || !QDir(suggestedDir).exists()) {
        suggestedDir = QFileInfo(m_originalPath).exists()
            ? QFileInfo(m_originalPath).absolutePath()
            : QDir::homePath();
    }

    QString suggestedFile = "result.png";
    if (QFileInfo(m_originalPath).exists()) {
        const QString base = QFileInfo(m_originalPath).completeBaseName();
        if (!base.isEmpty()) suggestedFile = base + "_result.png";
    }

    const QString suggestedPath = QDir(suggestedDir).filePath(suggestedFile);

    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this,
        "Сохранить изображение",
        suggestedPath,
        filter,
        &selectedFilter
    );
    if (path.isEmpty()) return;

    if (QFileInfo(path).suffix().isEmpty()) {
        QString ext = ".png";
        const QString f = selectedFilter.toLower();
        if (f.contains("*.jpg") || f.contains("*.jpeg")) ext = ".jpg";
        else if (f.contains("*.bmp")) ext = ".bmp";
        else if (f.contains("*.webp")) ext = ".webp";
        path += ext;
    }

    if (!m_srcResult.save(path)) {
        if (m_log) m_log->append("\nОшибка сохранения изображения: " + path);
        return;
    }

    st.setValue("paths/last_save_image_dir", QFileInfo(path).absolutePath());
    st.sync();

    if (m_log) m_log->append("\nИзображение сохранено: " + path);
}

bool ResultView::exportCSV(const QString& path, QString& err) const {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        err = "Cannot write: " + path;
        return false;
    }

    QStringList metaKeys;
    const QStringList cols = buildColumns(metaKeys);

    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);

    {
        QStringList header;
        for (const auto& c : cols) header << csvEscape(c);
        out << header.join(',') << "\n";
    }

    for (int i = 0; i < m_lastResult.detections.size(); ++i) {
        const auto& d = m_lastResult.detections[i];
        const int w = d.x2 - d.x1;
        const int h = d.y2 - d.y1;

        QStringList row;
        row << csvEscape(QString::number(i + 1));
        row << csvEscape(d.clsName);
        row << csvEscape(QString::number(d.conf, 'f', 6));
        row << csvEscape(QString::number(w));
        row << csvEscape(QString::number(h));

        for (const auto& k : metaKeys) row << csvEscape(jsonValueToText(d.meta.value(k)));
        out << row.join(',') << "\n";
    }

    f.close();
    return true;
}

bool ResultView::exportJSON(const QString& path, QString& err) const {
    QJsonObject root;
    root["generated_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["original_image_path"] = m_originalPath;
    root["module_id"] = m_lastResult.moduleId;
    root["device"] = m_lastResult.deviceUsed;
    root["image_w"] = m_lastResult.imageW;
    root["image_h"] = m_lastResult.imageH;
    root["timings_ms"] = m_lastResult.timingsMs;
    root["warnings"] = QJsonArray::fromStringList(m_lastResult.warnings);

    QJsonArray dets;
    for (int i = 0; i < m_lastResult.detections.size(); ++i) {
        const auto& d = m_lastResult.detections[i];
        QJsonObject o;
        o["id"] = i + 1;
        o["cls"] = d.clsName;
        o["conf"] = d.conf;
        o["x1"] = d.x1; o["y1"] = d.y1; o["x2"] = d.x2; o["y2"] = d.y2;
        o["w_px"] = (d.x2 - d.x1);
        o["h_px"] = (d.y2 - d.y1);
        o["meta"] = d.meta;
        dets.append(o);
    }
    root["detections"] = dets;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        err = "Cannot write: " + path;
        return false;
    }
    QJsonDocument doc(root);
    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

bool ResultView::exportTXT(const QString& path, QString& err) const {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        err = "Cannot write: " + path;
        return false;
    }

    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);

    out << m_log->toPlainText() << "\n\n";
    out << "TABLE:\n";

    // заголовки
    QStringList headers;
    for (int c = 0; c < m_tblDetections->columnCount(); ++c) headers << m_tblDetections->horizontalHeaderItem(c)->text();
    out << headers.join('\t') << "\n";

    // строки
    for (int r = 0; r < m_tblDetections->rowCount(); ++r) {
        QStringList row;
        for (int c = 0; c < m_tblDetections->columnCount(); ++c) {
            QTableWidgetItem* it = m_tblDetections->item(r, c);
            row << (it ? it->text() : "");
        }
        out << row.join('\t') << "\n";
    }

    f.close();
    return true;
}

bool ResultView::exportHTML(const QString& path, QString& err) const {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        err = "Cannot write: " + path;
        return false;
    }

    auto pixToDataUri = [](const QPixmap& pm) -> QString {
        if (pm.isNull()) return {};
        QByteArray ba;
        QBuffer buf(&ba);
        buf.open(QIODevice::WriteOnly);
        pm.save(&buf, "PNG");
        const QString b64 = QString::fromLatin1(ba.toBase64());
        return "data:image/png;base64," + b64;
    };

    const QString origUri = pixToDataUri(m_srcOriginal);
    const QString resUri  = pixToDataUri(m_srcResult);

    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);

    out << "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<style>"
           "body{font-family:Segoe UI,Arial,sans-serif;background:#111;color:#eee;padding:16px;}"
           "h2{margin:0 0 12px 0}"
           ".row{display:flex;gap:12px;flex-wrap:wrap}"
           "img{max-width:100%;border:1px solid #444;border-radius:6px}"
           "table{border-collapse:collapse;width:100%;margin-top:12px}"
           "th,td{border:1px solid #444;padding:6px 8px;font-size:13px}"
           "th{background:#222}"
           "</style></head><body>";

    out << "<h2>Отчёт</h2>";
    out << "<pre>" << m_log->toPlainText().toHtmlEscaped() << "</pre>";

    out << "<div class='row'>";
    if (!origUri.isEmpty()) out << "<div><div>Исходник</div><img src='" << origUri << "'></div>";
    if (!resUri.isEmpty())  out << "<div><div>Результат</div><img src='" << resUri << "'></div>";
    out << "</div>";

    out << "<table><thead><tr>";
    for (int c = 0; c < m_tblDetections->columnCount(); ++c) {
        out << "<th>" << m_tblDetections->horizontalHeaderItem(c)->text().toHtmlEscaped() << "</th>";
    }
    out << "</tr></thead><tbody>";

    for (int r = 0; r < m_tblDetections->rowCount(); ++r) {
        out << "<tr>";
        for (int c = 0; c < m_tblDetections->columnCount(); ++c) {
            QTableWidgetItem* it = m_tblDetections->item(r, c);
            out << "<td>" << (it ? it->text().toHtmlEscaped() : "") << "</td>";
        }
        out << "</tr>";
    }

    out << "</tbody></table>";
    out << "</body></html>";

    f.close();
    return true;
}

static QString sqlQuoteIdent(const QString& s) {
    QString out = s;
    out.replace("\"", "\"\"");
    return "\"" + out + "\"";
}

bool ResultView::exportSQLite(const QString& path, QString& err) const {
    const QString connName = "vk_export_" + QString::number(reinterpret_cast<quintptr>(this));

    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(path);
        if (!db.open()) {
            err = db.lastError().text();
            QSqlDatabase::removeDatabase(connName);
            return false;
        }

        QSqlQuery q(db);

        q.exec("DROP TABLE IF EXISTS meta");
        q.exec("DROP TABLE IF EXISTS detections");

        if (!q.exec("CREATE TABLE meta (k TEXT PRIMARY KEY, v TEXT)")) {
            err = q.lastError().text();
            db.close();
            QSqlDatabase::removeDatabase(connName);
            return false;
        }

        QStringList metaKeys;
        const QStringList cols = buildColumns(metaKeys);

        QStringList sqlCols;
        sqlCols << "id INTEGER" << "cls TEXT" << "conf REAL" << "w_px INTEGER" << "h_px INTEGER";
        for (const auto& k : metaKeys) sqlCols << (k + " TEXT");

        QString create = "CREATE TABLE detections (" + sqlCols.join(", ") + ")";
        if (!q.exec(create)) {
            err = q.lastError().text();
            db.close();
            QSqlDatabase::removeDatabase(connName);
            return false;
        }

        auto insMeta = [&](const QString& k, const QString& v) {
            QSqlQuery iq(db);
            iq.prepare("INSERT OR REPLACE INTO meta(k,v) VALUES(?,?)");
            iq.addBindValue(k);
            iq.addBindValue(v);
            iq.exec();
        };

        insMeta("generated_at", QDateTime::currentDateTime().toString(Qt::ISODate));
        insMeta("original_image_path", m_originalPath);
        insMeta("module_id", m_lastResult.moduleId);
        insMeta("device", m_lastResult.deviceUsed);
        insMeta("image_w", QString::number(m_lastResult.imageW));
        insMeta("image_h", QString::number(m_lastResult.imageH));

        // insert detections
        QStringList insCols;
        insCols << "id" << "cls" << "conf" << "w_px" << "h_px";
        insCols << metaKeys;

        QStringList ph;
        for (int i = 0; i < insCols.size(); ++i) ph << "?";

        QString ins = "INSERT INTO detections (" + insCols.join(", ") + ") VALUES (" + ph.join(", ") + ")";
        QSqlQuery iq(db);
        iq.prepare(ins);

        for (int i = 0; i < m_lastResult.detections.size(); ++i) {
            const auto& d = m_lastResult.detections[i];
            const int w = d.x2 - d.x1;
            const int h = d.y2 - d.y1;

            iq.addBindValue(i + 1);
            iq.addBindValue(d.clsName);
            iq.addBindValue(d.conf);
            iq.addBindValue(w);
            iq.addBindValue(h);

            for (const auto& k : metaKeys) iq.addBindValue(jsonValueToText(d.meta.value(k)));

            if (!iq.exec()) {
                err = iq.lastError().text();
                db.close();
                QSqlDatabase::removeDatabase(connName);
                return false;
            }
            iq.finish();
        }

        db.close();
    }

    QSqlDatabase::removeDatabase(connName);
    return true;
}

bool ResultView::exportXLSX(const QString& path, QString& err) const {
#ifndef VK_WITH_QXLSX
    err = "XLSX export disabled (no QXlsx).";
    return false;
#else
    QXlsx::Document x;

    // header
    for (int c = 0; c < m_table->columnCount(); ++c) {
        x.write(1, c + 1, m_table->horizontalHeaderItem(c)->text());
    }
    // rows
    for (int r = 0; r < m_table->rowCount(); ++r) {
        for (int c = 0; c < m_table->columnCount(); ++c) {
            QTableWidgetItem* it = m_table->item(r, c);
            x.write(r + 2, c + 1, it ? it->text() : "");
        }
    }

    if (!x.saveAs(path)) {
        err = "Cannot save xlsx: " + path;
        return false;
    }
    return true;
#endif
}

void ResultView::onExportData() {
    if (!m_hasResult) {
        m_log->append("\nНет данных для экспорта.");
        return;
    }

    const QString filter =
        "CSV (*.csv);;"
        "JSON (*.json);;"
        "XLSX (*.xlsx);;"
        "SQLite DB (*.db);;"
        "TXT (*.txt);;"
        "HTML (*.html)";

    const QString suggestedDir = QFileInfo(m_originalPath).exists()
        ? QFileInfo(m_originalPath).absolutePath()
        : QDir::homePath();

    const QString path = QFileDialog::getSaveFileName(this, "Экспорт данных", suggestedDir, filter);
    if (path.isEmpty()) return;

    QString err;
    bool ok = false;

    const QString lower = QFileInfo(path).suffix().toLower();
    if (lower == "csv") ok = exportCSV(path, err);
    else if (lower == "json") ok = exportJSON(path, err);
    else if (lower == "xlsx") ok = exportXLSX(path, err);
    else if (lower == "db") ok = exportSQLite(path, err);
    else if (lower == "txt") ok = exportTXT(path, err);
    else if (lower == "html" || lower == "htm") ok = exportHTML(path, err);
    else {
        err = "Unknown extension: " + lower;
        ok = false;
    }

    if (!ok) {
        m_log->append("\nОшибка экспорта: " + err);
        return;
    }

    m_log->append("\nЭкспорт выполнен: " + path);
}
QTextEdit* ResultView::logEdit() { return m_log; }

void ResultView::appendLog(const QString& s) {
    if (m_log) m_log->append(s);
}