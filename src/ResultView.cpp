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
#include <QProcess>
#include <QStandardPaths>
#include <QImageWriter>
#include <QRegularExpression>

#ifdef VK_WITH_QXLSX
#include <QXlsx/xlsxdocument.h>
#include <QXlsx/xlsxformat.h>
#endif
static QString b64All(const QByteArray& b) {
    return QString::fromLatin1(b.toBase64());
}

static QString bytesToTextLoose(const QByteArray& b) {
    QString s = QString::fromUtf8(b);
    if (s.contains(QChar(0xFFFD))) s = QString::fromLatin1(b);
    return s;
}

static inline quint16 rd16(const QByteArray& b, int off, bool le, bool* ok) {
    if (off < 0 || off + 2 > b.size()) { *ok = false; return 0; }
    const uchar a = (uchar)b[off];
    const uchar c = (uchar)b[off + 1];
    *ok = true;
    return le ? (quint16)(a | (c << 8)) : (quint16)((a << 8) | c);
}

static inline quint32 rd32(const QByteArray& b, int off, bool le, bool* ok) {
    if (off < 0 || off + 4 > b.size()) { *ok = false; return 0; }
    const uchar a = (uchar)b[off];
    const uchar c = (uchar)b[off + 1];
    const uchar d = (uchar)b[off + 2];
    const uchar e = (uchar)b[off + 3];
    *ok = true;
    return le ? (quint32)(a | (c << 8) | (d << 16) | (e << 24))
              : (quint32)((a << 24) | (c << 16) | (d << 8) | e);
}

static int typeSize(quint16 type) {
    switch (type) {
        case 1:  return 1;  // BYTE
        case 2:  return 1;  // ASCII
        case 3:  return 2;  // SHORT
        case 4:  return 4;  // LONG
        case 5:  return 8;  // RATIONAL
        case 7:  return 1;  // UNDEFINED
        case 9:  return 4;  // SLONG
        case 10: return 8;  // SRATIONAL
        case 11: return 4;  // FLOAT
        case 12: return 8;  // DOUBLE
    }
    return 0;
}

static QByteArray entryValueBytes(const QByteArray& tiff, bool le, int entryOff,
                                  quint16 type, quint32 count, quint32 valueOrOff) {
    Q_UNUSED(le)
    const int ts = typeSize(type);
    if (ts <= 0) return {};
    const qint64 total = (qint64)ts * (qint64)count;
    if (total <= 0) return {};

    if (total <= 4) {
        if (entryOff < 0 || entryOff + 12 > tiff.size()) return {};
        return tiff.mid(entryOff + 8, (int)total);
    }

    const int off = (int)valueOrOff;
    if (off < 0 || off + total > tiff.size()) return {};
    return tiff.mid(off, (int)total);
}

static QString tagNameGuess(quint16 tag, const QString& prefix) {
    const bool gps = prefix.contains("GPS", Qt::CaseInsensitive);
    if (gps) {
        switch (tag) {
            case 0x0000: return "GPSVersionID";
            case 0x0001: return "GPSLatitudeRef";
            case 0x0002: return "GPSLatitude";
            case 0x0003: return "GPSLongitudeRef";
            case 0x0004: return "GPSLongitude";
            case 0x0005: return "GPSAltitudeRef";
            case 0x0006: return "GPSAltitude";
            case 0x0007: return "GPSTimeStamp";
            case 0x0012: return "GPSMapDatum";
            case 0x001D: return "GPSDateStamp";
        }
    } else {
        switch (tag) {
            case 0x010E: return "ImageDescription";
            case 0x010F: return "Make";
            case 0x0110: return "Model";
            case 0x0112: return "Orientation";
            case 0x0131: return "Software";
            case 0x0132: return "DateTime";
            case 0x013B: return "Artist";
            case 0x8298: return "Copyright";

            case 0x8769: return "ExifIFDPointer";
            case 0x8825: return "GPSIFDPointer";
            case 0xA005: return "InteroperabilityIFDPointer";

            case 0x9003: return "DateTimeOriginal";
            case 0x9004: return "DateTimeDigitized";
            case 0x829A: return "ExposureTime";
            case 0x829D: return "FNumber";
            case 0x8827: return "ISOSpeedRatings";
            case 0x920A: return "FocalLength";
            case 0xA002: return "PixelXDimension";
            case 0xA003: return "PixelYDimension";
            case 0xA405: return "FocalLengthIn35mmFilm";
            case 0xA432: return "LensSpecification";
            case 0xA433: return "LensMake";
            case 0xA434: return "LensModel";

            case 0x014A: return "SubIFDs";
            case 0x0201: return "JPEGInterchangeFormat";
            case 0x0202: return "JPEGInterchangeFormatLength";

            case 0x9C9B: return "XPTitle";
            case 0x9C9C: return "XPComment";
            case 0x9C9D: return "XPAuthor";
            case 0x9C9E: return "XPKeywords";
            case 0x9C9F: return "XPSubject";
        }
    }
    return QString("tag_0x%1").arg(QString::number(tag, 16).toUpper().rightJustified(4, '0'));
}

static QString readNumberListU16(const QByteArray& b, bool le) {
    bool ok = false;
    const int n = b.size() / 2;
    QStringList out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const quint16 v = rd16(b, i * 2, le, &ok);
        if (!ok) return {};
        out << QString::number(v);
    }
    return out.join(", ");
}

static QString readNumberListU32(const QByteArray& b, bool le) {
    bool ok = false;
    const int n = b.size() / 4;
    QStringList out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const quint32 v = rd32(b, i * 4, le, &ok);
        if (!ok) return {};
        out << QString::number(v);
    }
    return out.join(", ");
}

static QString readNumberListS32(const QByteArray& b, bool le) {
    bool ok = false;
    const int n = b.size() / 4;
    QStringList out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const quint32 u = rd32(b, i * 4, le, &ok);
        if (!ok) return {};
        const qint32 v = (qint32)u;
        out << QString::number(v);
    }
    return out.join(", ");
}

static QString readRationalList(const QByteArray& b, bool le, bool signedRat) {
    bool ok = false;
    const int n = b.size() / 8;
    QStringList out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (!signedRat) {
            const quint32 num = rd32(b, i * 8, le, &ok); if (!ok) return {};
            const quint32 den = rd32(b, i * 8 + 4, le, &ok); if (!ok || den == 0) return {};
            out << QString::number((double)num / (double)den, 'f', 6);
        } else {
            const qint32 num = (qint32)rd32(b, i * 8, le, &ok); if (!ok) return {};
            const qint32 den = (qint32)rd32(b, i * 8 + 4, le, &ok); if (!ok || den == 0) return {};
            out << QString::number((double)num / (double)den, 'f', 6);
        }
    }
    return out.join(", ");
}

static QString entryToText(const QByteArray& tiff, bool le, int entryOff,
                           quint16 tag, quint16 type, quint32 count, quint32 valueOrOff) {
    const QByteArray vb = entryValueBytes(tiff, le, entryOff, type, count, valueOrOff);

    if (type == 2) {
        QByteArray s = vb;
        while (!s.isEmpty() && s.back() == '\0') s.chop(1);
        return bytesToTextLoose(s);
    }
    if (type == 3) return readNumberListU16(vb, le);
    if (type == 4) return readNumberListU32(vb, le);
    if (type == 9) return readNumberListS32(vb, le);
    if (type == 5) return readRationalList(vb, le, false);
    if (type == 10) return readRationalList(vb, le, true);

    if (type == 1 || type == 7) {
        if (tag == 0x9C9B || tag == 0x9C9C || tag == 0x9C9D || tag == 0x9C9E || tag == 0x9C9F) {
            if (vb.size() >= 2 && (vb.size() % 2 == 0)) {
                const char16_t* u = reinterpret_cast<const char16_t*>(vb.constData());
                const qsizetype n = vb.size() / 2;
                QString s = QString::fromUtf16(u, n);
                while (!s.isEmpty() && s.back() == QChar('\0')) s.chop(1);
                return s;
            }
        }
        if (!vb.isEmpty()) return b64All(vb);
        return QString("type=%1 count=%2").arg(type).arg(count);
    }

    if (type == 11) {
        if (vb.size() % 4 != 0) return QString("type=%1 count=%2").arg(type).arg(count);
        const int n = vb.size() / 4;
        QStringList out;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            float f;
            std::memcpy(&f, vb.constData() + i * 4, 4);
            out << QString::number((double)f, 'f', 6);
        }
        return out.join(", ");
    }

    if (type == 12) {
        if (vb.size() % 8 != 0) return QString("type=%1 count=%2").arg(type).arg(count);
        const int n = vb.size() / 8;
        QStringList out;
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            double d;
            std::memcpy(&d, vb.constData() + i * 8, 8);
            out << QString::number(d, 'f', 12);
        }
        return out.join(", ");
    }

    if (!vb.isEmpty()) return b64All(vb);
    return QString("type=%1 count=%2").arg(type).arg(count);
}

static bool parseIfdRecursive(const QByteArray& tiff, bool le, int ifdOff,
                              const QString& prefix, QSet<int>& visited, QJsonObject* outFlat) {
    if (ifdOff <= 0 || ifdOff >= tiff.size()) return false;
    if (visited.contains(ifdOff)) return true;
    visited.insert(ifdOff);

    bool ok = false;
    const quint16 n = rd16(tiff, ifdOff, le, &ok);
    if (!ok) return false;

    const int entriesBase = ifdOff + 2;
    const int entriesEnd = entriesBase + (int)n * 12;
    if (entriesEnd + 4 > tiff.size()) return false;

    int exifPtr = 0;
    int gpsPtr = 0;
    int interopPtr = 0;
    QVector<int> subIfds;

    for (quint16 i = 0; i < n; ++i) {
        const int eoff = entriesBase + (int)i * 12;
        if (eoff + 12 > tiff.size()) return false;

        const quint16 tag = rd16(tiff, eoff, le, &ok); if (!ok) return false;
        const quint16 type = rd16(tiff, eoff + 2, le, &ok); if (!ok) return false;
        const quint32 count = rd32(tiff, eoff + 4, le, &ok); if (!ok) return false;
        const quint32 vo = rd32(tiff, eoff + 8, le, &ok); if (!ok) return false;

        const QString name = tagNameGuess(tag, prefix);
        const QString key = prefix + name;
        const QString val = entryToText(tiff, le, eoff, tag, type, count, vo);
        outFlat->insert(key, val);

        if (tag == 0x8769) exifPtr = (int)vo;
        if (tag == 0x8825) gpsPtr = (int)vo;
        if (tag == 0xA005) interopPtr = (int)vo;

        if (tag == 0x014A) {
            const QByteArray vb = entryValueBytes(tiff, le, eoff, type, count, vo);
            if (!vb.isEmpty() && type == 4) {
                const int k = vb.size() / 4;
                for (int j = 0; j < k; ++j) {
                    const quint32 off = rd32(vb, j * 4, le, &ok);
                    if (ok && (int)off > 0) subIfds.push_back((int)off);
                }
            }
        }
    }

    const quint32 nextIfd = rd32(tiff, entriesEnd, le, &ok);
    if (!ok) return false;

    if (exifPtr > 0) parseIfdRecursive(tiff, le, exifPtr, "EXIF.ExifIFD.", visited, outFlat);
    if (gpsPtr > 0)  parseIfdRecursive(tiff, le, gpsPtr,  "EXIF.GPS.",    visited, outFlat);
    if (interopPtr > 0) parseIfdRecursive(tiff, le, interopPtr, "EXIF.Interop.", visited, outFlat);

    for (int si = 0; si < subIfds.size(); ++si) {
        parseIfdRecursive(tiff, le, subIfds[si], QString("EXIF.SubIFD%1.").arg(si), visited, outFlat);
    }

    if (prefix == "EXIF.IFD0." && nextIfd > 0) {
        parseIfdRecursive(tiff, le, (int)nextIfd, "EXIF.IFD1.", visited, outFlat);
    }

    return true;
}

static void extractExifFromApp1(const QByteArray& app1, QJsonObject* outFlat) {
    if (!app1.startsWith(QByteArray("Exif\0\0", 6))) return;
    const QByteArray tiff = app1.mid(6);
    if (tiff.size() < 8) return;

    const bool le = (tiff[0] == 'I' && tiff[1] == 'I');
    const bool be = (tiff[0] == 'M' && tiff[1] == 'M');
    if (!le && !be) return;

    bool ok = false;
    const quint16 magic = rd16(tiff, 2, le, &ok);
    if (!ok || magic != 42) return;

    const quint32 ifd0 = rd32(tiff, 4, le, &ok);
    if (!ok || (int)ifd0 <= 0 || (int)ifd0 >= tiff.size()) return;

    QSet<int> visited;
    parseIfdRecursive(tiff, le, (int)ifd0, "EXIF.IFD0.", visited, outFlat);

    const QString latRef = outFlat->value("EXIF.GPS.GPSLatitudeRef").toString().trimmed().toUpper();
    const QString lonRef = outFlat->value("EXIF.GPS.GPSLongitudeRef").toString().trimmed().toUpper();
    const QString latStr = outFlat->value("EXIF.GPS.GPSLatitude").toString();
    const QString lonStr = outFlat->value("EXIF.GPS.GPSLongitude").toString();

    auto parseDMS = [](const QString& s, double* out)->bool {
        const QStringList parts = s.split(',', Qt::SkipEmptyParts);
        if (parts.size() < 3) return false;
        bool ok1=false, ok2=false, ok3=false;
        const double d = parts[0].trimmed().toDouble(&ok1);
        const double m = parts[1].trimmed().toDouble(&ok2);
        const double sec = parts[2].trimmed().toDouble(&ok3);
        if (!ok1 || !ok2 || !ok3) return false;
        *out = d + m / 60.0 + sec / 3600.0;
        return true;
    };

    double lat = 0.0, lon = 0.0;
    if (parseDMS(latStr, &lat) && parseDMS(lonStr, &lon)) {
        if (latRef == "S") lat = -lat;
        if (lonRef == "W") lon = -lon;
        if (qIsFinite(lat) && qIsFinite(lon) && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
            outFlat->insert("GPS.lat_decimal", lat);
            outFlat->insert("GPS.lon_decimal", lon);
        }
    }
}

static void extractXmpFromApp1(const QByteArray& app1, QJsonObject* outFlat, int idx) {
    const QByteArray hdr = QByteArray("http://ns.adobe.com/xap/1.0/\0", 29);
    if (!app1.startsWith(hdr)) return;
    const QByteArray xml = app1.mid(hdr.size());
    outFlat->insert(QString("XMP[%1]").arg(idx), bytesToTextLoose(xml));
}

static void extractIccFromApp2(const QByteArray& app2, QMap<int, QByteArray>& iccParts, int& iccTotal) {
    const QByteArray hdr = QByteArray("ICC_PROFILE\0", 12);
    if (!app2.startsWith(hdr)) return;
    if (app2.size() < 14) return;
    const int seq = (uchar)app2[12];
    const int tot = (uchar)app2[13];
    if (seq <= 0 || tot <= 0) return;
    iccTotal = tot;
    iccParts[seq] = app2.mid(14);
}

static QJsonObject extractAllJpegMetadataFlat(const QString& imagePath) {
    QJsonObject out;

    QFile f(imagePath);
    if (!f.open(QIODevice::ReadOnly)) {
        out.insert("meta.error", "Cannot open file");
        return out;
    }
    const QByteArray jpg = f.readAll();
    f.close();

    if (jpg.size() < 4 || (uchar)jpg[0] != 0xFF || (uchar)jpg[1] != 0xD8) {
        out.insert("meta.error", "Not a JPEG");
        return out;
    }

    int app1Idx = 0;
    int xmpIdx = 0;
    int comIdx = 0;
    int app13Idx = 0;

    QMap<int, QByteArray> iccParts;
    int iccTotal = 0;

    int i = 2;
    while (i + 4 <= jpg.size()) {
        if ((uchar)jpg[i] != 0xFF) { i++; continue; }
        while (i < jpg.size() && (uchar)jpg[i] == 0xFF) i++;
        if (i >= jpg.size()) break;

        const uchar marker = (uchar)jpg[i++];
        if (marker == 0xD9 || marker == 0xDA) break;

        if (i + 2 > jpg.size()) break;
        const quint16 segLen = (quint16)(((uchar)jpg[i] << 8) | (uchar)jpg[i + 1]);
        i += 2;
        const int segDataLen = (int)segLen - 2;
        if (segDataLen < 0 || i + segDataLen > jpg.size()) break;

        const QByteArray seg = jpg.mid(i, segDataLen);
        i += segDataLen;

        if (marker == 0xE1) {
            out.insert(QString("JPEG.APP1[%1].bytes").arg(app1Idx), segDataLen);

            if (seg.startsWith(QByteArray("Exif\0\0", 6))) {
                extractExifFromApp1(seg, &out);
                out.insert(QString("JPEG.APP1[%1].type").arg(app1Idx), "EXIF");
            } else if (seg.startsWith(QByteArray("http://ns.adobe.com/xap/1.0/", 28))) {
                extractXmpFromApp1(seg, &out, xmpIdx++);
                out.insert(QString("JPEG.APP1[%1].type").arg(app1Idx), "XMP");
            } else {
                out.insert(QString("JPEG.APP1[%1].type").arg(app1Idx), "APP1");
                out.insert(QString("JPEG.APP1[%1].base64").arg(app1Idx), b64All(seg));
            }
            app1Idx++;
            continue;
        }

        if (marker == 0xE2) {
            extractIccFromApp2(seg, iccParts, iccTotal);
            continue;
        }

        if (marker == 0xED) {
            out.insert(QString("JPEG.APP13[%1].bytes").arg(app13Idx), segDataLen);
            out.insert(QString("JPEG.APP13[%1].base64").arg(app13Idx), b64All(seg));
            app13Idx++;
            continue;
        }

        if (marker == 0xFE) {
            out.insert(QString("JPEG.COM[%1]").arg(comIdx), bytesToTextLoose(seg));
            comIdx++;
            continue;
        }
    }

    if (iccTotal > 0 && !iccParts.isEmpty()) {
        QByteArray icc;
        bool okAll = true;
        for (int k = 1; k <= iccTotal; ++k) {
            if (!iccParts.contains(k)) { okAll = false; break; }
            icc += iccParts.value(k);
        }
        out.insert("ICC.total_parts", iccTotal);
        out.insert("ICC.have_parts", iccParts.size());
        if (okAll && !icc.isEmpty()) out.insert("ICC.profile.base64", b64All(icc));
    }

    return out;
}

static void flattenJsonToObject(const QString& prefix, const QJsonValue& v, QJsonObject* out) {
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            const QString p = prefix.isEmpty() ? it.key() : (prefix + "." + it.key());
            flattenJsonToObject(p, it.value(), out);
        }
        return;
    }
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        for (int i = 0; i < a.size(); ++i) {
            const QString p = prefix + "[" + QString::number(i) + "]";
            flattenJsonToObject(p, a[i], out);
        }
        return;
    }
    out->insert(prefix, v);
}

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

void ResultView::setPythonExe(const QString& pythonExe) {
    const QString v = pythonExe.trimmed();
    m_pythonExe = v.isEmpty() ? "python" : v;
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

    pm.setDevicePixelRatio(1.0);
    m_srcOriginal = pm;
    m_srcOriginal.setDevicePixelRatio(1.0);
}
void ResultView::setPreviewImage(const QString& originalPath) {
    m_originalPath = originalPath;
    loadOriginal(originalPath);

    applyScaled(m_imgOriginal, m_srcOriginal, m_keyOriginal, m_targetOriginal, m_scaledOriginal);

    m_srcResult = QPixmap();
    applyScaled(m_imgResult, m_srcResult, m_keyResult, m_targetResult, m_scaledResult);

    clearDynamicTableTabs();
    if (m_tblDetections) { m_tblDetections->setRowCount(0); m_tblDetections->setColumnCount(0); }
    if (m_tblExif) { m_tblExif->setRowCount(0); m_tblExif->setColumnCount(2); }

    clearPlotTabs();

    if (m_log) m_log->clear();
    if (m_tabs) m_tabs->setCurrentWidget(m_log);

    QJsonObject root;
    root.insert("file", extractAllJpegMetadataFlat(originalPath));
    m_lastResult.exif = root;

    rebuildExifTable();
}

void ResultView::setPreviewFromRunner(const QString& originalPath, const QString& displayPath, const QJsonObject& exifRoot) {
    m_originalPath = originalPath;

    const QString p = displayPath.trimmed().isEmpty() ? originalPath : displayPath;
    loadOriginal(p);

    applyScaled(m_imgOriginal, m_srcOriginal, m_keyOriginal, m_targetOriginal, m_scaledOriginal);

    m_srcResult = QPixmap();
    applyScaled(m_imgResult, m_srcResult, m_keyResult, m_targetResult, m_scaledResult);

    clearDynamicTableTabs();
    if (m_tblDetections) { m_tblDetections->setRowCount(0); m_tblDetections->setColumnCount(0); }
    if (m_tblExif) { m_tblExif->setRowCount(0); m_tblExif->setColumnCount(2); }

    clearPlotTabs();

    if (m_log) m_log->clear();
    if (m_tabs) m_tabs->setCurrentWidget(m_log);

    m_lastResult = ModuleResult();
    m_hasResult = false;

    m_lastResult.exif = exifRoot;
    rebuildExifTable();
}

void ResultView::setPreviewFromRaw(const QString& originalPath, const QImage& preview, const QJsonObject& exifRoot) {
    m_originalPath = originalPath;

    QPixmap pm = QPixmap::fromImage(preview);
    pm.setDevicePixelRatio(1.0);
    m_srcOriginal = pm;
    m_srcOriginal.setDevicePixelRatio(1.0);

    applyScaled(m_imgOriginal, m_srcOriginal, m_keyOriginal, m_targetOriginal, m_scaledOriginal);

    m_srcResult = QPixmap();
    applyScaled(m_imgResult, m_srcResult, m_keyResult, m_targetResult, m_scaledResult);

    clearDynamicTableTabs();
    if (m_tblDetections) { m_tblDetections->setRowCount(0); m_tblDetections->setColumnCount(0); }
    if (m_tblExif) { m_tblExif->setRowCount(0); m_tblExif->setColumnCount(2); }

    clearPlotTabs();

    if (m_log) m_log->clear();
    if (m_tabs) m_tabs->setCurrentWidget(m_log);

    m_lastResult = ModuleResult();
    m_hasResult = false;

    m_lastResult.exif = exifRoot;
    rebuildExifTable();
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
    QPixmap pm = QPixmap::fromImage(img);
    pm.setDevicePixelRatio(1.0);
    m_srcResult = pm;
    m_srcResult.setDevicePixelRatio(1.0);
}

static bool extractGpsLatLonFromExifRoot(const QJsonObject& exifRoot, double& lat, double& lon) {
    lat = 0.0;
    lon = 0.0;

    QJsonObject src = exifRoot;
    if (src.contains("runner") && src.value("runner").isObject()) src = src.value("runner").toObject();
    else if (src.contains("file") && src.value("file").isObject()) src = src.value("file").toObject();

    const QJsonObject gps = src.value("gps").toObject();
    const QJsonValue latv = gps.value("lat");
    const QJsonValue lonv = gps.value("lon");

    bool okLat = false;
    bool okLon = false;
    double lt = 0.0;
    double ln = 0.0;

    if (latv.isDouble()) { lt = latv.toDouble(); okLat = true; }
    else if (latv.isString()) { lt = latv.toString().toDouble(&okLat); }

    if (lonv.isDouble()) { ln = lonv.toDouble(); okLon = true; }
    else if (lonv.isString()) { ln = lonv.toString().toDouble(&okLon); }

    if (!okLat || !okLon) return false;
    if (!qIsFinite(lt) || !qIsFinite(ln)) return false;
    if (lt < -90.0 || lt > 90.0) return false;
    if (ln < -180.0 || ln > 180.0) return false;

    lat = lt;
    lon = ln;
    return true;
}

static void injectGpsToDetections(ModuleResult& r) {
    double lat = 0.0, lon = 0.0;
    const bool ok = extractGpsLatLonFromExifRoot(r.exif, lat, lon);

    for (auto& d : r.detections) {
        if (!d.meta.contains("gps_lat")) {
            if (ok) d.meta.insert("gps_lat", lat);
            else d.meta.insert("gps_lat", QString::fromUtf8("нет данных"));
        }
        if (!d.meta.contains("gps_lon")) {
            if (ok) d.meta.insert("gps_lon", lon);
            else d.meta.insert("gps_lon", QString::fromUtf8("нет данных"));
        }
    }
}

void ResultView::setResult(const ModuleResult& r) {
    // Сохраняем метаданные файла, которые были извлечены при выборе изображения (setPreviewImage)
    QJsonObject fileMetaSaved;
    if (m_lastResult.exif.contains("file") && m_lastResult.exif.value("file").isObject()) {
        fileMetaSaved = m_lastResult.exif.value("file").toObject();
    } else {
        fileMetaSaved = m_lastResult.exif;
    }

    m_lastResult = r;
    m_hasResult = true;

    // Объединяем: file (из JPEG) + runner (то, что вернул python)
    if (!fileMetaSaved.isEmpty()) {
        QJsonObject root;
        root.insert("file", fileMetaSaved);
        if (!m_lastResult.exif.isEmpty()) root.insert("runner", m_lastResult.exif);
        m_lastResult.exif = root;
    }
    injectGpsToDetections(m_lastResult);
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

    QJsonObject flat;
    flattenJsonToObject(QString(), m_lastResult.exif, &flat);

    if (flat.isEmpty()) {
        m_tblExif->setRowCount(1);
        m_tblExif->setItem(0, 0, new QTableWidgetItem("exif"));
        m_tblExif->setItem(0, 1, new QTableWidgetItem("нет данных"));
        m_tblExif->setSortingEnabled(true);
        return;
    }

    QStringList keys = flat.keys();
    std::sort(keys.begin(), keys.end());

    m_tblExif->setRowCount(keys.size());
    for (int i = 0; i < keys.size(); ++i) {
        const QString& k = keys[i];
        m_tblExif->setItem(i, 0, new QTableWidgetItem(k));
        m_tblExif->setItem(i, 1, new QTableWidgetItem(jsonValueToText(flat.value(k))));
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

    QString inExt = QFileInfo(m_originalPath).suffix().toLower();
    if (inExt == "jpeg") inExt = "jpg";
    if (inExt.isEmpty()) inExt = "png";

    QSet<QString> exts;
    const QList<QByteArray> fmts = QImageWriter::supportedImageFormats();
    for (const QByteArray& b : fmts) {
        QString e = QString::fromLatin1(b).toLower().trimmed();
        if (e.isEmpty()) continue;
        if (e == "jpeg") e = "jpg";
        if (e == "tif") e = "tiff";
        exts.insert(e);
    }

    // HEIC/HEIF добавляем как поддержку “через Python” (даже если Qt сам не умеет)
    exts.insert("heic");
    exts.insert("heif");

    auto patsFor = [](const QString& e) -> QStringList {
        if (e == "jpg")  return {"*.jpg", "*.jpeg", "*.jpe", "*.jfif", "*.jif"};
        if (e == "tiff") return {"*.tif", "*.tiff"};
        if (e == "jp2")  return {"*.jp2", "*.j2k", "*.j2c", "*.jpx", "*.jpf"};
        if (e == "heic") return {"*.heic"};
        if (e == "heif") return {"*.heif"};
        return { "*." + e };
    };

    auto titleFor = [](const QString& e) -> QString {
        if (e == "jpg")  return "JPEG";
        if (e == "png")  return "PNG";
        if (e == "bmp")  return "BMP";
        if (e == "webp") return "WEBP";
        if (e == "tiff") return "TIFF";
        if (e == "gif")  return "GIF";
        if (e == "jp2")  return "JPEG 2000";
        if (e == "ppm")  return "PPM";
        if (e == "pgm")  return "PGM";
        if (e == "pbm")  return "PBM";
        if (e == "xpm")  return "XPM";
        if (e == "xbm")  return "XBM";
        if (e == "ico")  return "ICO";
        if (e == "icns") return "ICNS";
        if (e == "heic") return "HEIC";
        if (e == "heif") return "HEIF";
        return e.toUpper();
    };

    // порядок: сначала расширение входного файла (если есть), затем популярные, затем остальное
    QStringList order;
    if (exts.contains(inExt)) order << inExt;

    const QStringList common = {
        "png","jpg","bmp","webp","tiff","gif","jp2","ico","icns","ppm","pgm","pbm","xpm","xbm","heic","heif"
    };
    for (const QString& e : common) {
        if (exts.contains(e) && !order.contains(e)) order << e;
    }

    QStringList rest = exts.values();
    std::sort(rest.begin(), rest.end());
    for (const QString& e : rest) {
        if (!order.contains(e)) order << e;
    }

    // формируем фильтры: сначала “как вход”, потом “все”, потом остальные
    QStringList filters;

    if (!order.isEmpty()) {
        const QString e0 = order.first();
        filters << (titleFor(e0) + " (" + patsFor(e0).join(' ') + ")");
    }

    QStringList allPats;
    for (const QString& e : order) allPats << patsFor(e);
    filters << ("Все поддерживаемые (" + allPats.join(' ') + ")");

    for (int i = 0; i < order.size(); ++i) {
        const QString e = order[i];
        const QString item = titleFor(e) + " (" + patsFor(e).join(' ') + ")";
        if (!filters.contains(item)) filters << item;
    }

    const QString filter = filters.join(";;");
    const QString iniPath = QDir(QCoreApplication::applicationDirPath()).filePath("ui.ini");
    QSettings st(iniPath, QSettings::IniFormat);

    QString suggestedDir = st.value("paths/last_save_image_dir", "").toString();
    if (suggestedDir.isEmpty() || !QDir(suggestedDir).exists()) {
        suggestedDir = QFileInfo(m_originalPath).exists()
            ? QFileInfo(m_originalPath).absolutePath()
            : QDir::homePath();
    }

    QString suggestedFile = "result." + inExt;
    if (QFileInfo(m_originalPath).exists()) {
        const QString base = QFileInfo(m_originalPath).completeBaseName();
        if (!base.isEmpty()) suggestedFile = base + "_result." + inExt;
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
        QString ext = "." + inExt;

        const QString f = selectedFilter.toLower();
        if (!f.startsWith("все поддерживаемые")) {
            QRegularExpression re(R"(\*\.(\w+))");
            QRegularExpressionMatch m = re.match(f);
            if (m.hasMatch()) {
                QString e = m.captured(1).toLower();
                if (e == "jpeg") e = "jpg";
                if (e == "tif") e = "tiff";
                ext = "." + e;
            }
        }
        path += ext;
    }

    QString outExt = QFileInfo(path).suffix().toLower();
    if (outExt == "jpeg") outExt = "jpg";

    const bool wantHeif = (outExt == "heic" || outExt == "heif");

    if (!wantHeif) {
        if (!m_srcResult.save(path)) {
            if (m_log) m_log->append("\nОшибка сохранения изображения: " + path);
            return;
        }
    } else {
        const QString tmpRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString tmpDir = QDir(tmpRoot.isEmpty() ? QDir::tempPath() : tmpRoot).filePath("traffic_save");
        QDir().mkpath(tmpDir);

        const QString tmpPng = QDir(tmpDir).filePath("tmp_save_result.png");
        if (!m_srcResult.save(tmpPng, "PNG")) {
            if (m_log) m_log->append("\nОшибка подготовки PNG для HEIC/HEIF: " + tmpPng);
            return;
        }

        const QString py = m_pythonExe.trimmed().isEmpty() ? "python" : m_pythonExe.trimmed();

        const QString pyCode =
            "import sys\n"
            "from PIL import Image\n"
            "from pillow_heif import register_heif_opener\n"
            "register_heif_opener()\n"
            "inp=sys.argv[1]\n"
            "outp=sys.argv[2]\n"
            "im=Image.open(inp)\n"
            "im.load()\n"
            "im.save(outp)\n";

        QProcess proc;
        proc.start(py, QStringList() << "-c" << pyCode << tmpPng << path);
        if (!proc.waitForFinished(-1) || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
            const QString err = QString::fromUtf8(proc.readAllStandardError());
            if (m_log) m_log->append("\nОшибка сохранения HEIC/HEIF через Python: " + err.trimmed());
            QFile::remove(tmpPng);
            return;
        }

        QFile::remove(tmpPng);
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