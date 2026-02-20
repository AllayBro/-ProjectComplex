#include "ResultView.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPixmap>
#include <QFileInfo>
#include <QJsonDocument>

ResultView::ResultView(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* imgs = new QHBoxLayout();
    m_imgAnnotated = new QLabel();
    m_imgAnnotated->setMinimumSize(520, 290);
    m_imgAnnotated->setScaledContents(true);
    m_imgAnnotated->setFrameShape(QFrame::Box);

    m_imgCleaned = new QLabel();
    m_imgCleaned->setMinimumSize(520, 290);
    m_imgCleaned->setScaledContents(true);
    m_imgCleaned->setFrameShape(QFrame::Box);

    imgs->addWidget(m_imgAnnotated, 1);
    imgs->addWidget(m_imgCleaned, 1);

    m_table = new QTableWidget();
    m_table->setColumnCount(8);
    m_table->setHorizontalHeaderLabels({"cls", "conf", "x1", "y1", "x2", "y2", "w", "meta"});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);

    m_log = new QTextEdit();
    m_log->setReadOnly(true);
    m_log->setMinimumHeight(140);

    root->addLayout(imgs);
    root->addWidget(m_table, 1);
    root->addWidget(m_log);
}

QTextEdit* ResultView::logEdit() { return m_log; }

void ResultView::clearAll() {
    m_imgAnnotated->clear();
    m_imgCleaned->clear();
    m_table->setRowCount(0);
    m_log->clear();
}

QString ResultView::metaToOneLine(const QJsonObject& o) {
    if (o.isEmpty()) return {};
    QJsonDocument d(o);
    return QString::fromUtf8(d.toJson(QJsonDocument::Compact));
}

void ResultView::setImageToLabel(QLabel* lbl, const QString& path) {
    if (path.isEmpty()) { lbl->clear(); return; }
    QFileInfo fi(path);
    if (!fi.exists()) { lbl->setText("Image not found:\n" + path); return; }
    QPixmap pm(path);
    if (pm.isNull()) { lbl->setText("Cannot load image:\n" + path); return; }
    lbl->setPixmap(pm);
}

void ResultView::setResult(const ModuleResult& r) {
    setImageToLabel(m_imgAnnotated, r.annotatedImagePath);
    setImageToLabel(m_imgCleaned, r.cleanedImagePath);

    m_table->setRowCount(r.detections.size());
    for (int i = 0; i < r.detections.size(); ++i) {
        const auto& d = r.detections[i];
        int w = d.x2 - d.x1;

        m_table->setItem(i, 0, new QTableWidgetItem(d.clsName));
        m_table->setItem(i, 1, new QTableWidgetItem(QString::number(d.conf, 'f', 4)));
        m_table->setItem(i, 2, new QTableWidgetItem(QString::number(d.x1)));
        m_table->setItem(i, 3, new QTableWidgetItem(QString::number(d.y1)));
        m_table->setItem(i, 4, new QTableWidgetItem(QString::number(d.x2)));
        m_table->setItem(i, 5, new QTableWidgetItem(QString::number(d.y2)));
        m_table->setItem(i, 6, new QTableWidgetItem(QString::number(w)));
        m_table->setItem(i, 7, new QTableWidgetItem(metaToOneLine(d.meta)));
    }

    QStringList lines;
    lines << ("module_id=" + r.moduleId)
          << ("device=" + r.deviceUsed);

    if (!r.timingsMs.isEmpty()) {
        QJsonDocument td(r.timingsMs);
        lines << ("timings_ms=" + QString::fromUtf8(td.toJson(QJsonDocument::Compact)));
    }
    if (!r.warnings.isEmpty()) lines << ("warnings=" + r.warnings.join(" | "));

    m_log->append(lines.join("\n"));
}