#include "ResultView.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPixmap>
#include <QFileInfo>
#include <QJsonDocument>

ResultView::ResultView(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* imgs = new QHBoxLayout();

    m_imgBefore = new QLabel();
    m_imgBefore->setMinimumSize(520, 290);
    m_imgBefore->setScaledContents(true);
    m_imgBefore->setFrameShape(QFrame::Box);
    m_imgBefore->setAlignment(Qt::AlignCenter);
    m_imgBefore->setText("До");

    m_imgAfter = new QLabel();
    m_imgAfter->setMinimumSize(520, 290);
    m_imgAfter->setScaledContents(true);
    m_imgAfter->setFrameShape(QFrame::Box);
    m_imgAfter->setAlignment(Qt::AlignCenter);
    m_imgAfter->setText("Итог");

    imgs->addWidget(m_imgBefore, 1);
    imgs->addWidget(m_imgAfter, 1);

    m_log = new QTextEdit();
    m_log->setReadOnly(true);
    m_log->setMinimumHeight(140);

    root->addLayout(imgs, 1);
    root->addWidget(m_log);
}

QTextEdit* ResultView::logEdit() { return m_log; }

void ResultView::clearAll() {
    m_imgBefore->clear();
    m_imgBefore->setText("До");

    m_imgAfter->clear();
    m_imgAfter->setText("Итог");

    m_log->clear();
}

void ResultView::setBeforeImage(const QString& path) {
    setImageToLabel(m_imgBefore, path);
}

QString ResultView::metaToOneLine(const QJsonObject& o) {
    if (o.isEmpty()) return {};
    QJsonDocument d(o);
    return QString::fromUtf8(d.toJson(QJsonDocument::Compact));
}

void ResultView::setImageToLabel(QLabel* lbl, const QString& path) {
    if (!lbl) return;

    if (path.isEmpty()) {
        lbl->clear();
        return;
    }

    QFileInfo fi(path);
    if (!fi.exists()) {
        lbl->setText("Image not found:\n" + path);
        return;
    }

    QPixmap pm(path);
    if (pm.isNull()) {
        lbl->setText("Cannot load image:\n" + path);
        return;
    }

    lbl->setPixmap(pm);
}

void ResultView::setResult(const ModuleResult& r) {
    // Итог: если cleaned есть — показываем его, иначе annotated.
    const QString afterPath = !r.cleanedImagePath.isEmpty() ? r.cleanedImagePath : r.annotatedImagePath;
    setImageToLabel(m_imgAfter, afterPath);

    QStringList lines;
    lines << ("module_id=" + r.moduleId)
          << ("device=" + r.deviceUsed)
          << ("detections=" + QString::number(r.detections.size()));

    if (!r.timingsMs.isEmpty()) {
        QJsonDocument td(r.timingsMs);
        lines << ("timings_ms=" + QString::fromUtf8(td.toJson(QJsonDocument::Compact)));
    }
    if (!r.warnings.isEmpty()) lines << ("warnings=" + r.warnings.join(" | "));

    m_log->append(lines.join("\n"));
}