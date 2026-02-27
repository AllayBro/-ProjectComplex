#include "MapTab.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QTextEdit>
#include <QFileInfo>
#include <QFile>
#include <QPixmap>
#include <QVariantMap>
#include <QVariantList>
#include <QFrame>
#include <QtGlobal>
#include <QStringList>
#include <QtQuickWidgets/QQuickWidget>
#include <QtQuick/QQuickItem>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlComponent>
#include <QMetaObject>
#include <QCoreApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QTimer>
#include <QUrl>

static QString qmlMapView() {
    return QString::fromUtf8(R"QML(
import QtQuick
import QtLocation
import QtPositioning

Item {
    id: root
    width: 640
    height: 480

    property var pointsModel: []
    property bool offlineMode: false
    property string offlineDir: ""
    property string cacheDir: ""
    property string userAgent: "vk_qt_app"

    signal markerClicked(string imagePath)

    Plugin {
        id: osmOnline
        name: "osm"
        PluginParameter { name: "osm.useragent"; value: root.userAgent }
        PluginParameter { name: "osm.mapping.cache.directory"; value: root.cacheDir }
    }

    Plugin {
        id: osmOffline
        name: "osm"
        PluginParameter { name: "osm.useragent"; value: root.userAgent }
        PluginParameter { name: "osm.mapping.cache.directory"; value: root.cacheDir }
        PluginParameter { name: "osm.mapping.offline.directory"; value: root.offlineDir }
        PluginParameter { name: "osm.mapping.providersrepository.disabled"; value: true }
    }

    function centerOn(lat, lon) {
        map.center = QtPositioning.coordinate(lat, lon)
        if (map.zoomLevel < 13) map.zoomLevel = 13
    }

    Map {
        id: map
        anchors.fill: parent
        plugin: root.offlineMode ? osmOffline : osmOnline
        zoomLevel: 4
        center: QtPositioning.coordinate(20, 0)

        MapItemView {
            model: root.pointsModel
            delegate: MapQuickItem {
                coordinate: QtPositioning.coordinate(modelData.lat, modelData.lon)
                anchorPoint.x: marker.width / 2
                anchorPoint.y: marker.height

                sourceItem: Rectangle {
                    id: marker
                    width: 14
                    height: 14
                    radius: 7
                    color: "#e53935"
                    border.color: "white"
                    border.width: 2

                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.markerClicked(modelData.imagePath)
                    }
                }
            }
        }
    }
}
)QML");
}

MapTab::MapTab(const AppConfig& cfg, QWidget* parent)
    : QWidget(parent), m_cfg(cfg)
{
    auto* root = new QVBoxLayout(this);

    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel("Карта:"));
    m_netStatus = new QLabel();
    top->addWidget(m_netStatus, 1);
    top->addStretch(1);
    root->addLayout(top);

    auto* split = new QSplitter(Qt::Horizontal, this);

    m_quick = new QQuickWidget(this);
    m_quick->setResizeMode(QQuickWidget::SizeRootObjectToView);

    auto* right = new QWidget(this);
    auto* rlay = new QVBoxLayout(right);

    m_preview = new QLabel();
    m_preview->setMinimumHeight(220);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFrameShape(QFrame::Box);

    m_info = new QTextEdit();
    m_info->setReadOnly(true);
    m_info->setLineWrapMode(QTextEdit::NoWrap);

    rlay->addWidget(m_preview, 1);
    rlay->addWidget(m_info, 2);

    split->addWidget(m_quick);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    root->addWidget(split, 1);

    initQml();

    // online сначала
    m_onlineOk = true;
    m_probeSeen = false;
    applyEffectiveModeToQml();
    updateNetLabel();

    m_nam = new QNetworkAccessManager(this);

    m_probeTimer = new QTimer(this);
    int interval = m_cfg.map.probeIntervalMs;
    if (interval < 1000) interval = 5000; // Защита от слишком частых запросов
    m_probeTimer->setInterval(interval);
    connect(m_probeTimer, &QTimer::timeout, this, &MapTab::probeNetworkNow);
    m_probeTimer->start();

    m_probeTimeout = new QTimer(this);
    m_probeTimeout->setSingleShot(true);
    connect(m_probeTimeout, &QTimer::timeout, this, &MapTab::onProbeTimeout);

    if (!m_cfg.map.probeUrl.isEmpty()) {
        QTimer::singleShot(100, this, &MapTab::probeNetworkNow);
    } else {
        m_onlineOk = true;
        m_probeSeen = true;
        updateNetLabel();
    }
}

void MapTab::initQml()
{
    const QString qml = qmlMapView();
    
    m_quick->setSource(QUrl()); 
    m_quick->engine()->clearComponentCache();

    QQmlComponent* comp = new QQmlComponent(m_quick->engine(), this);
    comp->setData(qml.toUtf8(), QUrl(QStringLiteral("inmemory:/MapView.qml")));

    if (comp->isError()) {
        m_netStatus->setText("QML Error: Component error");
        m_info->setPlainText(comp->errorString());
        comp->deleteLater();
        return;
    }

    QObject* obj = comp->create();
    if (!obj) {
        m_netStatus->setText("QML Error: Create failed");
        comp->deleteLater();
        return;
    }

    // Важно: QQuickWidget берет на себя владение объектом через setContent
    m_quick->setContent(QUrl(QStringLiteral("inmemory:/MapView.qml")), comp, obj);

    QQuickItem* ro = m_quick->rootObject();
    if (!ro) {
        m_netStatus->setText("QML error: rootObject is null");
        return;
    }

    connect(ro, SIGNAL(markerClicked(QString)), this, SLOT(onMarkerClicked(QString)));
    
    ro->setProperty("userAgent", m_cfg.map.userAgent);
    ro->setProperty("cacheDir", m_cfg.map.cacheDir);
    ro->setProperty("offlineDir", m_cfg.map.offlineTilesDir);
    
    applyEffectiveModeToQml();
}

void MapTab::applyEffectiveModeToQml()
{
    QQuickItem* ro = m_quick->rootObject();
    if (!ro) return;

    const bool offline = m_cfg.map.startOffline ? true : (!m_onlineOk && m_probeSeen);
    ro->setProperty("offlineMode", offline);
    ro->setProperty("offlineDir", m_cfg.map.offlineTilesDir);
    ro->setProperty("cacheDir", m_cfg.map.cacheDir);

    pushModelToQml();
}

void MapTab::updateNetLabel()
{
    const bool offline = m_cfg.map.startOffline ? true : (!m_onlineOk && m_probeSeen);
    const QString mode = offline ? "OFFLINE" : "ONLINE";
    const QString net = m_probeSeen ? (m_onlineOk ? "internet=ok" : "internet=fail") : "internet=checking";
    m_netStatus->setText(mode + "  |  " + net);
}

void MapTab::probeNetworkNow()
{
    if (!m_nam) return;

    if (m_probeReply) {
        m_probeReply->abort();
        m_probeReply->deleteLater();
        m_probeReply = nullptr;
    }

    QNetworkRequest req(QUrl(m_cfg.map.probeUrl));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    m_probeReply = m_nam->head(req);
    connect(m_probeReply, &QNetworkReply::finished, this, &MapTab::onProbeFinished);

    m_probeTimeout->start(m_cfg.map.probeTimeoutMs);
}

void MapTab::onProbeTimeout()
{
    if (!m_probeReply) return;
    m_probeReply->abort();
}

void MapTab::onProbeFinished()
{
    m_probeTimeout->stop();

    if (!m_probeReply) return;

    bool ok = (m_probeReply->error() == QNetworkReply::NoError);
    const QVariant st = m_probeReply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (ok && st.isValid()) {
        const int code = st.toInt();
        ok = (code >= 200 && code < 400);
    }

    m_probeSeen = true;
    m_onlineOk = ok;

    m_probeReply->deleteLater();
    m_probeReply = nullptr;

    applyEffectiveModeToQml();
    updateNetLabel();
}

void MapTab::pushModelToQml()
{
    QQuickItem* ro = m_quick->rootObject();
    if (!ro) return;

    QVariantList list;
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        const Item& v = it.value();
        if (!v.hasGps) continue;
        QVariantMap m;
        m.insert("imagePath", v.imagePath);
        m.insert("lat", v.lat);
        m.insert("lon", v.lon);
        list.push_back(m);
    }

    ro->setProperty("pointsModel", list);
}

void MapTab::selectItem(const QString& imagePath)
{
    m_selected = imagePath;

    auto it = m_items.find(imagePath);
    if (it == m_items.end()) return;

    const Item& v = it.value();
    updateInfoPanel(v);

    if (v.hasGps) {
        QQuickItem* ro = m_quick->rootObject();
        if (ro) {
            QMetaObject::invokeMethod(ro, "centerOn",
                                      Q_ARG(QVariant, v.lat),
                                      Q_ARG(QVariant, v.lon));
        }
    }
}

void MapTab::updateInfoPanel(const Item& it)
{
    QPixmap pm(it.imagePath);
    if (!pm.isNull()) {
        m_preview->setPixmap(pm.scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        m_preview->clear();
    }

    QStringList s;
    s << ("file=" + it.imagePath);

    if (it.hasGps) {
        s << QString("gps=%1, %2").arg(it.lat, 0, 'f', 8).arg(it.lon, 0, 'f', 8);
    } else {
        s << "gps=нет координат";
    }

    if (!it.make.isEmpty()) s << ("make=" + it.make);
    if (!it.model.isEmpty()) s << ("model=" + it.model);
    if (!it.dateTime.isEmpty()) s << ("datetime=" + it.dateTime);
    if (!it.dateTimeOriginal.isEmpty()) s << ("datetime_original=" + it.dateTimeOriginal);

    if (it.hasResult) {
        s << "---- result ----";
        s << ("module_id=" + it.result.moduleId);
        s << ("device=" + it.result.deviceUsed);
        s << ("detections=" + QString::number(it.result.detections.size()));
    }

    m_info->setPlainText(s.join("\n"));
}

void MapTab::onMarkerClicked(const QString& imagePath)
{
    selectItem(imagePath);
}

void MapTab::onImageSelected(const QString& imagePath)
{
    const QString p = imagePath.trimmed();
    if (p.isEmpty()) return;

    QFileInfo fi(p);
    if (!fi.exists() || !fi.isFile()) return;

    Item it;
    it.imagePath = fi.absoluteFilePath();
    readExifMini(it.imagePath, it);

    m_items.insert(it.imagePath, it);
    pushModelToQml();
    selectItem(it.imagePath);
}

void MapTab::onResultReady(const QString& imagePath, const ModuleResult& r)
{
    const QString p = imagePath.trimmed();
    if (p.isEmpty()) return;

    auto it = m_items.find(p);
    if (it == m_items.end()) {
        Item ni;
        ni.imagePath = p;
        readExifMini(ni.imagePath, ni);
        it = m_items.insert(p, ni);
    }

    it->hasResult = true;
    it->result = r;

    if (m_selected == p)
        updateInfoPanel(*it);
}

// Мини-EXIF GPS (JPEG APP1 Exif)
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

static bool findExifApp1(const QByteArray& jpg, int* tiffBase, int* tiffLen) {
    if (jpg.size() < 4) return false;
    if ((uchar)jpg[0] != 0xFF || (uchar)jpg[1] != 0xD8) return false;

    int i = 2;
    while (i + 4 <= jpg.size()) {
        if ((uchar)jpg[i] != 0xFF) { i++; continue; }
        const uchar marker = (uchar)jpg[i + 1];
        if (marker == 0xDA || marker == 0xD9) break;

        const int lenOff = i + 2;
        if (lenOff + 2 > jpg.size()) return false;
        const quint16 segLen = (quint16)(((uchar)jpg[lenOff] << 8) | (uchar)jpg[lenOff + 1]);
        if (segLen < 2) return false;

        const int segStart = i + 4;
        const int segDataLen = (int)segLen - 2;
        if (segStart + segDataLen > jpg.size()) return false;

        if (marker == 0xE1 && segDataLen >= 6) {
            const QByteArray hdr = jpg.mid(segStart, 6);
            if (hdr == QByteArray("Exif\0\0", 6)) {
                *tiffBase = segStart + 6;
                *tiffLen = segDataLen - 6;
                return (*tiffLen > 8);
            }
        }
        i = segStart + segDataLen;
    }
    return false;
}

static bool readAsciiValue(const QByteArray& tiff, bool le, int entryOff,
                           quint16 type, quint32 count, quint32 valueOrOff, QString* out) {
    Q_UNUSED(le)
    if (type != 2 || count == 0) return false;

    QByteArray raw;
    const int n = (int)count;
    if (n <= 4) {
        if ((entryOff + 8 + n) > (tiff.size())) return false;
        raw = tiff.mid(entryOff + 8, n);
    } else {
        const int off = (int)valueOrOff;
        if (off < 0 || off + n > tiff.size()) return false;
        raw = tiff.mid(off, n);
    }

    while (!raw.isEmpty() && (raw.back() == '\0')) raw.chop(1);
    *out = QString::fromUtf8(raw);
    return !out->isEmpty();
}

static bool findIfdEntry(const QByteArray& tiff, bool le, int ifdOff, quint16 tag,
                         quint16* type, quint32* count, quint32* valueOrOff, int* entryAbsOff) {
    bool ok = false;
    const quint16 n = rd16(tiff, ifdOff, le, &ok);
    if (!ok) return false;

    int p = ifdOff + 2;
    for (quint16 i = 0; i < n; ++i) {
        if (p + 12 > tiff.size()) return false;

        const quint16 t = rd16(tiff, p, le, &ok); if (!ok) return false;
        const quint16 ty = rd16(tiff, p + 2, le, &ok); if (!ok) return false;
        const quint32 cn = rd32(tiff, p + 4, le, &ok); if (!ok) return false;
        const quint32 vo = rd32(tiff, p + 8, le, &ok); if (!ok) return false;

        if (t == tag) {
            *type = ty;
            *count = cn;
            *valueOrOff = vo;
            if (entryAbsOff) *entryAbsOff = p;
            return true;
        }
        p += 12;
    }
    return false;
}

static bool readRational3(const QByteArray& tiff, bool le, quint32 off, double* deg, double* min, double* sec) {
    bool ok = false;
    const int o = (int)off;
    if (o < 0 || o + 24 > tiff.size()) return false;

    auto rat = [&](int k)->double{
        const quint32 num = rd32(tiff, o + k * 8, le, &ok); if (!ok) return 0.0;
        const quint32 den = rd32(tiff, o + k * 8 + 4, le, &ok); if (!ok || den == 0) return 0.0;
        return (double)num / (double)den;
    };

    *deg = rat(0);
    *min = rat(1);
    *sec = rat(2);
    return true;
}

bool MapTab::readExifMini(const QString& imagePath, Item& out) {
    out.hasGps = false;
    out.make.clear();
    out.model.clear();
    out.dateTime.clear();
    out.dateTimeOriginal.clear();

    QFile f(imagePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray jpg = f.readAll();
    f.close();

    int tiffBaseAbs = 0;
    int tiffLen = 0;
    if (!findExifApp1(jpg, &tiffBaseAbs, &tiffLen)) return false;

    const QByteArray tiff = jpg.mid(tiffBaseAbs, tiffLen);
    if (tiff.size() < 8) return false;

    const bool le = (tiff[0] == 'I' && tiff[1] == 'I');
    const bool be = (tiff[0] == 'M' && tiff[1] == 'M');
    if (!le && !be) return false;

    bool ok = false;
    const quint16 magic = rd16(tiff, 2, le, &ok);
    if (!ok || magic != 42) return false;

    const quint32 ifd0 = rd32(tiff, 4, le, &ok);
    if (!ok || (int)ifd0 >= tiff.size()) return false;

    quint16 type = 0; quint32 count = 0; quint32 vo = 0; int eoff = 0;

    if (findIfdEntry(tiff, le, (int)ifd0, 0x010F, &type, &count, &vo, &eoff)) {
        QString s;
        if (readAsciiValue(tiff, le, eoff, type, count, vo, &s)) out.make = s;
    }
    if (findIfdEntry(tiff, le, (int)ifd0, 0x0110, &type, &count, &vo, &eoff)) {
        QString s;
        if (readAsciiValue(tiff, le, eoff, type, count, vo, &s)) out.model = s;
    }
    if (findIfdEntry(tiff, le, (int)ifd0, 0x0132, &type, &count, &vo, &eoff)) {
        QString s;
        if (readAsciiValue(tiff, le, eoff, type, count, vo, &s)) out.dateTime = s;
    }

    if (findIfdEntry(tiff, le, (int)ifd0, 0x8769, &type, &count, &vo, nullptr)) {
        const int exifIfd = (int)vo;
        if (exifIfd > 0 && exifIfd < tiff.size()) {
            if (findIfdEntry(tiff, le, exifIfd, 0x9003, &type, &count, &vo, &eoff)) {
                QString s;
                if (readAsciiValue(tiff, le, eoff, type, count, vo, &s)) out.dateTimeOriginal = s;
            }
        }
    }

    if (findIfdEntry(tiff, le, (int)ifd0, 0x8825, &type, &count, &vo, nullptr)) {
        const int gpsIfd = (int)vo;
        if (gpsIfd > 0 && gpsIfd < tiff.size()) {
            QString latRef, lonRef;
            quint16 ty = 0; quint32 cn = 0; quint32 v = 0; int eo = 0;

            if (findIfdEntry(tiff, le, gpsIfd, 1, &ty, &cn, &v, &eo)) {
                QString s;
                if (readAsciiValue(tiff, le, eo, ty, cn, v, &s)) latRef = s;
            }
            if (findIfdEntry(tiff, le, gpsIfd, 3, &ty, &cn, &v, &eo)) {
                QString s;
                if (readAsciiValue(tiff, le, eo, ty, cn, v, &s)) lonRef = s;
            }

            double d=0, m=0, s=0;
            double d2=0, m2=0, s2=0;

            if (findIfdEntry(tiff, le, gpsIfd, 2, &ty, &cn, &v, nullptr) && ty == 5 && cn == 3) {
                if (!readRational3(tiff, le, v, &d, &m, &s)) return true;
            } else return true;

            if (findIfdEntry(tiff, le, gpsIfd, 4, &ty, &cn, &v, nullptr) && ty == 5 && cn == 3) {
                if (!readRational3(tiff, le, v, &d2, &m2, &s2)) return true;
            } else return true;

            double lat = d + m / 60.0 + s / 3600.0;
            double lon = d2 + m2 / 60.0 + s2 / 3600.0;

            const QString lr = latRef.trimmed().toUpper();
            const QString gr = lonRef.trimmed().toUpper();
            if (lr == "S") lat = -lat;
            if (gr == "W") lon = -lon;

            if (qIsFinite(lat) && qIsFinite(lon) && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
                out.hasGps = true;
                out.lat = lat;
                out.lon = lon;
            }
        }
    }

    return true;
}
