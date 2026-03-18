#include "MapTab.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QTextEdit>
#include <QFileInfo>
#include <QFile>
#include <QDir>
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
#include <QProcess>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPushButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <QtGlobal>
#include <QtMath>
#include <cmath>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonParseError>

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
    property string userAgent: "traffic"

    property bool hasSelectedPoint: false
    property double selectedLat: 0.0
    property double selectedLon: 0.0
    property bool hasSelectedDirection: false
    property double selectedAzimuthDeg: 0.0
    property double uncertaintyM: 0.0

    property bool hasExifPoint: false
    property double exifLat: 0.0
    property double exifLon: 0.0

    property bool hasManualPoint: false
    property double manualLat: 0.0
    property double manualLon: 0.0

    property bool hasRefinedPoint: false
    property double refinedLat: 0.0
    property double refinedLon: 0.0

    property bool hasVehiclePoint: false
    property double vehicleLat: 0.0
    property double vehicleLon: 0.0
    property string vehicleLabel: ""

    signal markerClicked(string imagePath)
    signal mapClicked(double lat, double lon)

    function centerOn(lat, lon) {
        var activeMap = root.offlineMode ? mapOffline : mapOnline
        activeMap.center = QtPositioning.coordinate(lat, lon)
        if (activeMap.zoomLevel < 16)
            activeMap.zoomLevel = 16
    }

    function setActiveMapType(mapObject) {
        if (mapObject.supportedMapTypes && mapObject.supportedMapTypes.length > 0)
            mapObject.activeMapType = mapObject.supportedMapTypes[mapObject.supportedMapTypes.length - 1]
    }

    function destinationCoordinate(lat, lon, bearingDeg, distanceM) {
        var R = 6378137.0
        var brng = bearingDeg * Math.PI / 180.0
        var lat1 = lat * Math.PI / 180.0
        var lon1 = lon * Math.PI / 180.0
        var ang = distanceM / R

        var sinLat1 = Math.sin(lat1)
        var cosLat1 = Math.cos(lat1)
        var sinAng = Math.sin(ang)
        var cosAng = Math.cos(ang)

        var lat2 = Math.asin(sinLat1 * cosAng + cosLat1 * sinAng * Math.cos(brng))
        var lon2 = lon1 + Math.atan2(Math.sin(brng) * sinAng * cosLat1,
                                     cosAng - sinLat1 * Math.sin(lat2))

        return QtPositioning.coordinate(lat2 * 180.0 / Math.PI, lon2 * 180.0 / Math.PI)
    }

    function rebuildSelectionGeometry() {
        var pathOnline = []
        var pathOffline = []

        if (root.hasSelectedPoint && root.hasSelectedDirection) {
            var start = QtPositioning.coordinate(root.selectedLat, root.selectedLon)
            var end = destinationCoordinate(root.selectedLat, root.selectedLon, root.selectedAzimuthDeg, 80.0)
            pathOnline = [start, end]
            pathOffline = [start, end]
        }

        cameraLineOnline.path = pathOnline
        cameraLineOffline.path = pathOffline
    }

    onHasSelectedPointChanged: rebuildSelectionGeometry()
    onSelectedLatChanged: rebuildSelectionGeometry()
    onSelectedLonChanged: rebuildSelectionGeometry()
    onHasSelectedDirectionChanged: rebuildSelectionGeometry()
    onSelectedAzimuthDegChanged: rebuildSelectionGeometry()

    Plugin {
        id: osmOnline
        name: "osm"
        PluginParameter { name: "osm.useragent"; value: root.userAgent }
        PluginParameter { name: "osm.mapping.cache.directory"; value: root.cacheDir }
        PluginParameter { name: "osm.mapping.providersrepository.disabled"; value: true }
        PluginParameter { name: "osm.mapping.custom.host"; value: "https://tile.openstreetmap.org/" }
        PluginParameter { name: "osm.mapping.custom.mapcopyright"; value: "OpenStreetMap" }
        PluginParameter { name: "osm.mapping.custom.datacopyright"; value: "© OpenStreetMap contributors" }
        PluginParameter { name: "osm.mapping.cache.disk.size"; value: 104857600 }
        PluginParameter { name: "osm.mapping.cache.disk.cost_strategy"; value: "bytesize" }
        PluginParameter { name: "osm.mapping.prefetching_style"; value: "TwoNeighbourLayers" }
    }

    Plugin {
        id: osmOffline
        name: "osm"
        PluginParameter { name: "osm.useragent"; value: root.userAgent }
        PluginParameter { name: "osm.mapping.cache.directory"; value: root.cacheDir }
        PluginParameter { name: "osm.mapping.offline.directory"; value: root.offlineDir }
        PluginParameter { name: "osm.mapping.providersrepository.disabled"; value: true }
        PluginParameter { name: "osm.mapping.custom.host"; value: "https://tile.openstreetmap.org/" }
        PluginParameter { name: "osm.mapping.custom.mapcopyright"; value: "OpenStreetMap" }
        PluginParameter { name: "osm.mapping.custom.datacopyright"; value: "© OpenStreetMap contributors" }
        PluginParameter { name: "osm.mapping.cache.disk.size"; value: 104857600 }
        PluginParameter { name: "osm.mapping.cache.disk.cost_strategy"; value: "bytesize" }
    }

    Component {
        id: markerDelegate
        MapQuickItem {
            coordinate: QtPositioning.coordinate(modelData.lat, modelData.lon)
            anchorPoint.x: 9
            anchorPoint.y: 18
            sourceItem: Rectangle {
                width: modelData.selected ? 18 : 14
                height: modelData.selected ? 18 : 14
                radius: width / 2
                color: modelData.sourceType === "refined"
                       ? "#43a047"
                       : (modelData.sourceType === "manual"
                          ? "#1976d2"
                          : (modelData.sourceType === "estimated" ? "#f9a825" : "#e53935"))
                border.color: "white"
                border.width: modelData.selected ? 3 : 2
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.markerClicked(modelData.imagePath)
                }
            }
        }
    }

    Component {
        id: vehicleMarkerComponent
        Item {
            Column {
                spacing: 2
                anchors.horizontalCenter: parent.horizontalCenter
                Rectangle {
                    width: 18
                    height: 18
                    radius: 9
                    color: "#8e24aa"
                    border.color: "white"
                    border.width: 2
                    anchors.horizontalCenter: parent.horizontalCenter
                }
                Rectangle {
                    visible: root.vehicleLabel.length > 0
                    color: "#ffffff"
                    opacity: 0.9
                    radius: 4
                    border.color: "#8e24aa"
                    border.width: 1
                    anchors.horizontalCenter: parent.horizontalCenter
                    Text {
                        anchors.margins: 4
                        anchors.fill: parent
                        text: root.vehicleLabel
                        color: "#202020"
                        font.pixelSize: 11
                        wrapMode: Text.NoWrap
                    }
                }
            }
        }
    }

    Map {
        id: mapOnline
        anchors.fill: parent
        plugin: osmOnline
        visible: !root.offlineMode
        zoomLevel: 10
        center: QtPositioning.coordinate(55.7558, 37.6173)
        property var startCentroid

        MapItemView { model: root.pointsModel; delegate: markerDelegate }

        MapCircle {
            visible: root.hasSelectedPoint && root.uncertaintyM > 0
            center: QtPositioning.coordinate(root.selectedLat, root.selectedLon)
            radius: root.uncertaintyM
            color: "#1e88e533"
            border.width: 2
            border.color: "#1e88e5"
        }

        MapQuickItem {
            visible: root.hasExifPoint
            coordinate: QtPositioning.coordinate(root.exifLat, root.exifLon)
            anchorPoint.x: 10
            anchorPoint.y: 10
            sourceItem: Rectangle { width: 20; height: 20; radius: 10; color: "#e53935"; border.color: "white"; border.width: 2 }
        }

        MapQuickItem {
            visible: root.hasManualPoint
            coordinate: QtPositioning.coordinate(root.manualLat, root.manualLon)
            anchorPoint.x: 10
            anchorPoint.y: 10
            sourceItem: Rectangle { width: 20; height: 20; radius: 10; color: "#1976d2"; border.color: "white"; border.width: 2 }
        }

        MapQuickItem {
            visible: root.hasRefinedPoint
            coordinate: QtPositioning.coordinate(root.refinedLat, root.refinedLon)
            anchorPoint.x: 12
            anchorPoint.y: 12
            sourceItem: Rectangle { width: 24; height: 24; radius: 12; color: "#43a047"; border.color: "white"; border.width: 3 }
        }

        MapQuickItem {
            visible: root.hasVehiclePoint
            coordinate: QtPositioning.coordinate(root.vehicleLat, root.vehicleLon)
            anchorPoint.x: 9
            anchorPoint.y: 24
            sourceItem: vehicleMarkerComponent
        }

        MapPolyline {
            id: cameraLineOnline
            line.width: 4
            line.color: "#43a047"
            path: []
        }

        Component.onCompleted: root.setActiveMapType(mapOnline)

        TapHandler {
            target: null
            onTapped: function(eventPoint) {
                var c = mapOnline.toCoordinate(eventPoint.position, false)
                root.mapClicked(c.latitude, c.longitude)
            }
        }

        PinchHandler {
            target: null
            onActiveChanged: if (active) mapOnline.startCentroid = mapOnline.toCoordinate(centroid.position, false)
            onScaleChanged: function(delta) {
                mapOnline.zoomLevel += Math.log(delta) / Math.log(2)
                mapOnline.alignCoordinateToPoint(mapOnline.startCentroid, centroid.position)
            }
            grabPermissions: PointerHandler.TakeOverForbidden
        }

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            rotationScale: 1/120
            property: "zoomLevel"
        }

        DragHandler {
            target: null
            onTranslationChanged: function(delta) { mapOnline.pan(-delta.x, -delta.y) }
        }
    }

    Map {
        id: mapOffline
        anchors.fill: parent
        plugin: osmOffline
        visible: root.offlineMode
        zoomLevel: 10
        center: QtPositioning.coordinate(55.7558, 37.6173)
        property var startCentroid

        MapItemView { model: root.pointsModel; delegate: markerDelegate }

        MapCircle {
            visible: root.hasSelectedPoint && root.uncertaintyM > 0
            center: QtPositioning.coordinate(root.selectedLat, root.selectedLon)
            radius: root.uncertaintyM
            color: "#1e88e533"
            border.width: 2
            border.color: "#1e88e5"
        }

        MapQuickItem {
            visible: root.hasExifPoint
            coordinate: QtPositioning.coordinate(root.exifLat, root.exifLon)
            anchorPoint.x: 10
            anchorPoint.y: 10
            sourceItem: Rectangle { width: 20; height: 20; radius: 10; color: "#e53935"; border.color: "white"; border.width: 2 }
        }

        MapQuickItem {
            visible: root.hasManualPoint
            coordinate: QtPositioning.coordinate(root.manualLat, root.manualLon)
            anchorPoint.x: 10
            anchorPoint.y: 10
            sourceItem: Rectangle { width: 20; height: 20; radius: 10; color: "#1976d2"; border.color: "white"; border.width: 2 }
        }

        MapQuickItem {
            visible: root.hasRefinedPoint
            coordinate: QtPositioning.coordinate(root.refinedLat, root.refinedLon)
            anchorPoint.x: 12
            anchorPoint.y: 12
            sourceItem: Rectangle { width: 24; height: 24; radius: 12; color: "#43a047"; border.color: "white"; border.width: 3 }
        }

        MapQuickItem {
            visible: root.hasVehiclePoint
            coordinate: QtPositioning.coordinate(root.vehicleLat, root.vehicleLon)
            anchorPoint.x: 9
            anchorPoint.y: 24
            sourceItem: vehicleMarkerComponent
        }

        MapPolyline {
            id: cameraLineOffline
            line.width: 4
            line.color: "#43a047"
            path: []
        }

        Component.onCompleted: root.setActiveMapType(mapOffline)

        TapHandler {
            target: null
            onTapped: function(eventPoint) {
                var c = mapOffline.toCoordinate(eventPoint.position, false)
                root.mapClicked(c.latitude, c.longitude)
            }
        }

        PinchHandler {
            target: null
            onActiveChanged: if (active) mapOffline.startCentroid = mapOffline.toCoordinate(centroid.position, false)
            onScaleChanged: function(delta) {
                mapOffline.zoomLevel += Math.log(delta) / Math.log(2)
                mapOffline.alignCoordinateToPoint(mapOffline.startCentroid, centroid.position)
            }
            grabPermissions: PointerHandler.TakeOverForbidden
        }

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            rotationScale: 1/120
            property: "zoomLevel"
        }

        DragHandler {
            target: null
            onTranslationChanged: function(delta) { mapOffline.pan(-delta.x, -delta.y) }
        }
    }
}
)QML");
}

static QString absPathLocalMap(const QString& appDir, const QString& relOrAbs) {
    QFileInfo fi(relOrAbs);
    if (fi.isAbsolute()) return fi.absoluteFilePath();
    return QDir(appDir).filePath(relOrAbs);
}

static QString tempBaseDirLocalMap() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (d.isEmpty()) d = QDir::tempPath();
    if (d.isEmpty()) d = QDir::homePath();
    return d;
}

static bool readExifViaRunnerPreviewMap(const AppConfig& cfg,
                                        const QString& inputPath,
                                        QJsonObject& outExif,
                                        QString& err) {
    outExif = QJsonObject();
    err.clear();

    const QString py = cfg.pythonExe.trimmed().isEmpty() ? "python" : cfg.pythonExe.trimmed();
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString runner = absPathLocalMap(appDir, cfg.runnerScript);
    const QString basePyCfg = absPathLocalMap(appDir, cfg.pythonConfigJson);

    if (!QFileInfo(runner).exists()) { err = "runner.py не найден: " + runner; return false; }
    if (!QFileInfo(basePyCfg).exists()) { err = "python config не найден: " + basePyCfg; return false; }
    if (!QFileInfo(inputPath).exists()) { err = "input не найден: " + inputPath; return false; }

    const QString workDir = QDir(tempBaseDirLocalMap()).filePath("vk_qt_app_map_exif_preview");
    QDir wd(workDir);
    if (wd.exists() && !wd.removeRecursively()) { err = "Не удалось очистить temp dir: " + workDir; return false; }
    if (!QDir().mkpath(workDir)) { err = "Не удалось создать temp dir: " + workDir; return false; }

    const QString resultJson = QDir(workDir).filePath("preview.json");

    QProcess proc;
    QStringList args;
    args << "-u" << "-X" << "faulthandler"
         << runner
         << "--task" << "preview"
         << "--input" << inputPath
         << "--output-dir" << workDir
         << "--device" << "auto"
         << "--config" << basePyCfg
         << "--result-json" << resultJson;

    proc.start(py, args);
    if (!proc.waitForFinished(-1)) {
        err = "preview: процесс не завершился";
        wd.removeRecursively();
        return false;
    }

    if (!QFileInfo(resultJson).exists()) {
        err = "preview: result_json не создан. stderr=" + QString::fromUtf8(proc.readAllStandardError()).trimmed();
        wd.removeRecursively();
        return false;
    }

    QFile f(resultJson);
    if (!f.open(QIODevice::ReadOnly)) {
        err = "preview: не открыть result_json: " + resultJson;
        wd.removeRecursively();
        return false;
    }

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    f.close();

    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        err = "preview: JSON parse error: " + pe.errorString();
        wd.removeRecursively();
        return false;
    }

    const QJsonObject root = doc.object();
    outExif = root.value("exif").toObject();

    wd.removeRecursively();
    return !outExif.isEmpty();
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

    auto* geoRow1 = new QHBoxLayout();
    m_btnSetCameraPoint = new QPushButton("Точка камеры", this);
    m_btnSetDirection = new QPushButton("Направление", this);
    m_btnClearGeoRef = new QPushButton("Сброс", this);
    m_btnSaveGeoRef = new QPushButton("Сохранить", this);
    geoRow1->addWidget(m_btnSetCameraPoint);
    geoRow1->addWidget(m_btnSetDirection);
    geoRow1->addWidget(m_btnClearGeoRef);
    geoRow1->addWidget(m_btnSaveGeoRef);
    rlay->addLayout(geoRow1);

    auto* geoRow2 = new QHBoxLayout();
    geoRow2->addWidget(new QLabel("Источник:"));
    m_locationSource = new QComboBox(this);
    m_locationSource->addItem("EXIF", "exif");
    m_locationSource->addItem("Ручная", "manual");
    m_locationSource->addItem("Оценочная", "estimated");
    geoRow2->addWidget(m_locationSource, 1);
    geoRow2->addWidget(new QLabel("Радиус, м:"));
    m_uncertainty = new QDoubleSpinBox(this);
    m_uncertainty->setRange(0.0, 5000.0);
    m_uncertainty->setDecimals(1);
    m_uncertainty->setSingleStep(5.0);
    m_uncertainty->setValue(15.0);
    geoRow2->addWidget(m_uncertainty);
    rlay->addLayout(geoRow2);

    m_geoStatus = new QLabel(this);
    m_geoStatus->setWordWrap(true);
    rlay->addWidget(m_geoStatus);

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

    connect(m_btnSetCameraPoint, &QPushButton::clicked, this, &MapTab::onSetCameraPointMode);
    connect(m_btnSetDirection, &QPushButton::clicked, this, &MapTab::onSetDirectionMode);
    connect(m_btnClearGeoRef, &QPushButton::clicked, this, &MapTab::onClearGeoRef);
    connect(m_btnSaveGeoRef, &QPushButton::clicked, this, &MapTab::onSaveGeoRef);
    connect(m_locationSource, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int){ onLocationSourceChanged(); });
    connect(m_uncertainty, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &MapTab::onUncertaintyChanged);

    initQml();
    updateGeoControlsFromSelection();

    m_onlineOk = true;
    m_probeSeen = false;
    applyEffectiveModeToQml();
    updateNetLabel();

    m_nam = new QNetworkAccessManager(this);

    m_probeTimer = new QTimer(this);
    int interval = m_cfg.map.probeIntervalMs;
    if (interval < 1000) interval = 5000;
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
    const QString tempQmlPath = QDir(QCoreApplication::applicationDirPath()).filePath("temp_map.qml");

    // Сохраняем QML во временный файл для надежной загрузки
    QFile f(tempQmlPath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(qml.toUtf8());
        f.close();
    }

    m_quick->setSource(QUrl::fromLocalFile(tempQmlPath));

    // Проверка статуса загрузки
    if (m_quick->status() == QQuickWidget::Error) {
        QStringList errs;
        for (const auto& e : m_quick->errors()) errs << e.toString();
        m_netStatus->setText("QML Error");
        m_info->setPlainText(errs.join("\n"));
        return;
    }

    // Если загрузка асинхронная, подождем готовности
    auto setupRoot = [this]() {
        QQuickItem* ro = m_quick->rootObject();
        if (!ro) return;

        connect(ro, SIGNAL(markerClicked(QString)), this, SLOT(onMarkerClicked(QString)));
        connect(ro, SIGNAL(mapClicked(double,double)), this, SLOT(onMapClicked(double,double)));
        ro->setProperty("userAgent", m_cfg.map.userAgent);
        ro->setProperty("cacheDir", m_cfg.map.cacheDir);
        ro->setProperty("offlineDir", m_cfg.map.offlineTilesDir);
        applyEffectiveModeToQml();
        syncSelectedToQml();
    };

    if (m_quick->status() == QQuickWidget::Ready) {
        setupRoot();
    } else {
        connect(m_quick, &QQuickWidget::statusChanged, this, [this, setupRoot](QQuickWidget::Status s){
            if (s == QQuickWidget::Ready) setupRoot();
            else if (s == QQuickWidget::Error) {
                QStringList errs;
                for (const auto& e : m_quick->errors()) errs << e.toString();
                m_info->setPlainText("Async QML Error:\n" + errs.join("\n"));
            }
        });
    }
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
    const QString source = offline ? "cache/offline" : "tile.openstreetmap.org";
    m_netStatus->setText(mode + "  |  " + net + "  |  " + source);
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
bool MapTab::itemDisplayCoords(const Item& it, double& lat, double& lon, QString* source)
{
    if (it.hasRefinedCamera) {
        lat = it.refinedLat;
        lon = it.refinedLon;
        if (source) *source = QStringLiteral("refined");
        return true;
    }

    if (it.hasCameraPoint) {
        lat = it.cameraLat;
        lon = it.cameraLon;
        if (source) *source = it.locationSource.isEmpty() ? QStringLiteral("manual") : it.locationSource;
        return true;
    }

    if (it.hasGps) {
        lat = it.lat;
        lon = it.lon;
        if (source) *source = QStringLiteral("exif");
        return true;
    }

    if (source) source->clear();
    lat = 0.0;
    lon = 0.0;
    return false;
}

QString MapTab::defaultLocationSource(const Item& it)
{
    if (!it.locationSource.trimmed().isEmpty()) return it.locationSource.trimmed();
    if (it.hasRefinedCamera) return QStringLiteral("refined");
    if (it.hasCameraPoint) return QStringLiteral("manual");
    if (it.hasGps) return QStringLiteral("exif");
    return QStringLiteral("estimated");
}

QString MapTab::geoRefSidecarPath(const QString& imagePath)
{
    return imagePath + QStringLiteral(".georef.json");
}

bool MapTab::loadGeoRef(const QString& imagePath, Item& out, QString* err)
{
    if (err) err->clear();
    out.geoRefPath = geoRefSidecarPath(imagePath);

    QFile f(out.geoRefPath);
    if (!f.exists()) return false;
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("Не удалось открыть georef: ") + out.geoRefPath;
        return false;
    }

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    f.close();
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) *err = QStringLiteral("Ошибка JSON georef: ") + pe.errorString();
        return false;
    }

    const QJsonObject o = doc.object();
    const QString storedImagePath = o.value(QStringLiteral("image_path")).toString().trimmed();
    if (!storedImagePath.isEmpty() && QFileInfo(storedImagePath).absoluteFilePath() != QFileInfo(imagePath).absoluteFilePath()) {
        if (err) *err = QStringLiteral("georef относится к другому файлу");
        return false;
    }

    auto readDouble = [&](const char* key, double& dst, bool& ok) {
        ok = false;
        const QJsonValue v = o.value(QString::fromLatin1(key));
        if (v.isDouble()) {
            dst = v.toDouble();
            ok = qIsFinite(dst);
        } else if (v.isString()) {
            dst = v.toString().toDouble(&ok);
        }
    };

    bool okLat = false;
    bool okLon = false;
    double lat = 0.0;
    double lon = 0.0;
    readDouble("camera_lat", lat, okLat);
    readDouble("camera_lon", lon, okLon);
    if (okLat && okLon && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
        out.hasCameraPoint = true;
        out.cameraLat = lat;
        out.cameraLon = lon;
    }

    bool okAz = false;
    double az = 0.0;
    readDouble("camera_azimuth_deg", az, okAz);
    if (okAz) {
        out.hasCameraAzimuth = true;
        out.cameraAzimuthDeg = normalizeAzimuth(az);
    }

    bool okUnc = false;
    double unc = 0.0;
    readDouble("uncertainty_m", unc, okUnc);
    if (okUnc && unc >= 0.0) out.uncertaintyM = unc;

    const QString source = o.value(QStringLiteral("location_source")).toString().trimmed();
    if (!source.isEmpty()) out.locationSource = source;

    return true;
}

bool MapTab::saveGeoRef(const Item& it, QString* err)
{
    if (err) err->clear();

    const QString path = it.geoRefPath.isEmpty() ? geoRefSidecarPath(it.imagePath) : it.geoRefPath;

    if (!it.hasCameraPoint && !it.hasCameraAzimuth) {
        QFile::remove(path);
        return true;
    }

    QJsonObject o;
    o.insert(QStringLiteral("image_path"), it.imagePath);
    if (it.hasCameraPoint) {
        o.insert(QStringLiteral("camera_lat"), it.cameraLat);
        o.insert(QStringLiteral("camera_lon"), it.cameraLon);
    }
    if (it.hasCameraAzimuth) {
        o.insert(QStringLiteral("camera_azimuth_deg"), it.cameraAzimuthDeg);
    }
    o.insert(QStringLiteral("location_source"), defaultLocationSource(it));
    o.insert(QStringLiteral("uncertainty_m"), it.uncertaintyM);
    if (it.hasGps) {
        o.insert(QStringLiteral("exif_lat"), it.lat);
        o.insert(QStringLiteral("exif_lon"), it.lon);
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("Не удалось сохранить georef: ") + path;
        return false;
    }

    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

double MapTab::normalizeAzimuth(double value)
{
    if (!qIsFinite(value)) return 0.0;
    value = std::fmod(value, 360.0);
    if (value < 0.0) value += 360.0;
    return value;
}

double MapTab::azimuthDegrees(double lat1, double lon1, double lat2, double lon2)
{
    const double phi1 = qDegreesToRadians(lat1);
    const double phi2 = qDegreesToRadians(lat2);
    const double dLon = qDegreesToRadians(lon2 - lon1);
    const double y = std::sin(dLon) * std::cos(phi2);
    const double x = std::cos(phi1) * std::sin(phi2) -
                     std::sin(phi1) * std::cos(phi2) * std::cos(dLon);
    return normalizeAzimuth(qRadiansToDegrees(std::atan2(y, x)));
}
void MapTab::syncSelectedToQml()
{
    QQuickItem* ro = m_quick->rootObject();
    if (!ro) return;

    ro->setProperty("hasSelectedPoint", false);
    ro->setProperty("hasSelectedDirection", false);
    ro->setProperty("uncertaintyM", 0.0);

    ro->setProperty("hasExifPoint", false);
    ro->setProperty("hasManualPoint", false);
    ro->setProperty("hasRefinedPoint", false);
    ro->setProperty("hasVehiclePoint", false);
    ro->setProperty("vehicleLabel", QString());

    if (m_selected.isEmpty() || !m_items.contains(m_selected)) return;

    const Item& it = m_items[m_selected];

    if (it.hasGps) {
        ro->setProperty("hasExifPoint", true);
        ro->setProperty("exifLat", it.lat);
        ro->setProperty("exifLon", it.lon);
    }

    if (it.hasCameraPoint) {
        ro->setProperty("hasManualPoint", true);
        ro->setProperty("manualLat", it.cameraLat);
        ro->setProperty("manualLon", it.cameraLon);
    }

    if (it.hasRefinedCamera) {
        ro->setProperty("hasRefinedPoint", true);
        ro->setProperty("refinedLat", it.refinedLat);
        ro->setProperty("refinedLon", it.refinedLon);
    }

    if (it.hasVehicleGeo) {
        ro->setProperty("hasVehiclePoint", true);
        ro->setProperty("vehicleLat", it.vehicleLat);
        ro->setProperty("vehicleLon", it.vehicleLon);

        QStringList label;
        if (it.vehicleDistanceM > 0.0)
            label << QString("dist=%1 m").arg(it.vehicleDistanceM, 0, 'f', 1);
        label << QString("conf=%1").arg(it.vehicleConfidence, 0, 'f', 2);
        label << (it.vehicleNeedsManualReview ? "manual=true" : "manual=false");
        ro->setProperty("vehicleLabel", label.join(" | "));
    }

    double lat = 0.0;
    double lon = 0.0;
    if (!itemDisplayCoords(it, lat, lon, nullptr)) return;

    ro->setProperty("hasSelectedPoint", true);
    ro->setProperty("selectedLat", lat);
    ro->setProperty("selectedLon", lon);

    const bool hasDirection = it.hasRefinedAzimuth || it.hasCameraAzimuth;
    const double azimuth = it.hasRefinedAzimuth ? it.refinedAzimuthDeg : it.cameraAzimuthDeg;
    const double unc = it.hasRefinedCamera ? it.refinedUncertaintyM : it.uncertaintyM;

    ro->setProperty("hasSelectedDirection", hasDirection);
    ro->setProperty("selectedAzimuthDeg", azimuth);
    ro->setProperty("uncertaintyM", unc);
}

void MapTab::updateGeoStatus(const QString& text)
{
    if (!text.isEmpty()) {
        m_geoStatus->setText(text);
        return;
    }

    switch (m_editMode) {
    case EditMode::SetCameraPoint:
        m_geoStatus->setText("Режим: щёлкните по карте, чтобы задать положение камеры.");
        break;
    case EditMode::SetCameraDirection:
        m_geoStatus->setText("Режим: щёлкните по карте второй точкой, чтобы задать азимут камеры.");
        break;
    case EditMode::Idle:
    default:
        if (m_selected.isEmpty())
            m_geoStatus->setText("Снимок не выбран.");
        else
            m_geoStatus->setText("Геопривязка доступна для выбранного снимка.");
        break;
    }
}

void MapTab::updateGeoControlsFromSelection()
{
    const bool hasSelection = !m_selected.isEmpty() && m_items.contains(m_selected);

    m_btnSetCameraPoint->setEnabled(hasSelection);
    m_btnClearGeoRef->setEnabled(hasSelection);
    m_btnSaveGeoRef->setEnabled(hasSelection);

    bool canSetDirection = false;
    if (hasSelection) {
        const Item& it = m_items[m_selected];
        double lat = 0.0;
        double lon = 0.0;
        canSetDirection = itemDisplayCoords(it, lat, lon, nullptr);
    }
    m_btnSetDirection->setEnabled(canSetDirection);

    m_updatingControls = true;
    if (hasSelection) {
        const Item& it = m_items[m_selected];
        const QString src = defaultLocationSource(it);
        const int idx = m_locationSource->findData(src);
        m_locationSource->setCurrentIndex(idx >= 0 ? idx : 0);
        m_uncertainty->setValue(it.uncertaintyM > 0.0 ? it.uncertaintyM : 15.0);
    } else {
        m_locationSource->setCurrentIndex(0);
        m_uncertainty->setValue(15.0);
    }
    m_locationSource->setEnabled(hasSelection);
    m_uncertainty->setEnabled(hasSelection);
    m_updatingControls = false;

    updateGeoStatus();
}
void MapTab::pushModelToQml()
{
    QQuickItem* ro = m_quick->rootObject();
    if (!ro) return;

    QVariantList list;
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        const Item& v = it.value();

        double lat = 0.0;
        double lon = 0.0;
        QString source;
        if (!itemDisplayCoords(v, lat, lon, &source)) continue;

        QVariantMap m;
        m.insert("imagePath", v.imagePath);
        m.insert("lat", lat);
        m.insert("lon", lon);
        m.insert("sourceType", source);
        m.insert("selected", v.imagePath == m_selected);
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

    pushModelToQml();
    syncSelectedToQml();
    updateGeoControlsFromSelection();
    updateInfoPanel(v);

    double mapLat = 0.0;
    double mapLon = 0.0;
    bool hasPoint = false;

    if (v.hasVehicleGeo) {
        mapLat = v.vehicleLat;
        mapLon = v.vehicleLon;
        hasPoint = true;
    } else if (v.hasRefinedCamera) {
        mapLat = v.refinedLat;
        mapLon = v.refinedLon;
        hasPoint = true;
    } else if (v.hasCameraPoint) {
        mapLat = v.cameraLat;
        mapLon = v.cameraLon;
        hasPoint = true;
    } else if (v.hasGps) {
        mapLat = v.lat;
        mapLon = v.lon;
        hasPoint = true;
    }

    if (hasPoint) {
        QQuickItem* ro = m_quick->rootObject();
        if (ro) {
            QMetaObject::invokeMethod(ro, "centerOn",
                                      Q_ARG(QVariant, mapLat),
                                      Q_ARG(QVariant, mapLon));
        }
    }
}

void MapTab::applyRunnerExif(const QJsonObject& exifObj, Item& out)
{
    out.exif = exifObj;

    if (exifObj.isEmpty()) return;

    out.hasGps = false;
    out.lat = 0.0;
    out.lon = 0.0;

    const QJsonObject gps = exifObj.value("gps").toObject();
    const QJsonValue latv = gps.value("lat");
    const QJsonValue lonv = gps.value("lon");

    bool okLat = false;
    bool okLon = false;
    double lat = 0.0;
    double lon = 0.0;

    if (latv.isDouble()) { lat = latv.toDouble(); okLat = true; }
    else if (latv.isString()) { lat = latv.toString().toDouble(&okLat); }

    if (lonv.isDouble()) { lon = lonv.toDouble(); okLon = true; }
    else if (lonv.isString()) { lon = lonv.toString().toDouble(&okLon); }

    if (okLat && okLon && qIsFinite(lat) && qIsFinite(lon) && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
        out.hasGps = true;
        out.lat = lat;
        out.lon = lon;
    }

    const QJsonObject data = exifObj.value("data").toObject();

    auto setIf = [&](const char* key, QString& dst) {
        const QJsonValue v = data.value(QString::fromLatin1(key));
        const QString s = v.toString().trimmed();
        if (!s.isEmpty()) dst = s;
    };

    setIf("Make", out.make);
    setIf("Model", out.model);
    setIf("DateTime", out.dateTime);
    setIf("DateTimeOriginal", out.dateTimeOriginal);
}

void MapTab::applyCameraRefine(const QJsonObject& artifactsObj, Item& out)
{
    out.hasRefinedCamera = false;
    out.refinedLat = 0.0;
    out.refinedLon = 0.0;
    out.hasRefinedAzimuth = false;
    out.refinedAzimuthDeg = 0.0;
    out.refinedUncertaintyM = 0.0;
    out.refinedConfidence = 0.0;
    out.refinedNeedsManualReview = false;
    out.refinedMethod.clear();
    out.refinedSeedSource.clear();
    out.cameraRefineJsonPath.clear();

    if (artifactsObj.isEmpty()) return;

    out.cameraRefineJsonPath = artifactsObj.value("camera_refine_json_path").toString().trimmed();

    const QJsonObject refine = artifactsObj.value("camera_refine").toObject();
    if (refine.isEmpty()) return;

    auto readDouble = [](const QJsonObject& o, const char* key, double& dst, bool& ok) {
        ok = false;
        const QJsonValue v = o.value(QString::fromLatin1(key));
        if (v.isDouble()) {
            dst = v.toDouble();
            ok = qIsFinite(dst);
        } else if (v.isString()) {
            dst = v.toString().toDouble(&ok);
        }
    };

    bool okLat = false;
    bool okLon = false;
    double lat = 0.0;
    double lon = 0.0;
    readDouble(refine, "camera_refined_lat", lat, okLat);
    readDouble(refine, "camera_refined_lon", lon, okLon);

    if (okLat && okLon && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
        out.hasRefinedCamera = true;
        out.refinedLat = lat;
        out.refinedLon = lon;
    }

    bool okAz = false;
    double az = 0.0;
    readDouble(refine, "camera_refined_azimuth_deg", az, okAz);
    if (okAz) {
        out.hasRefinedAzimuth = true;
        out.refinedAzimuthDeg = az;
    }

    bool okUnc = false;
    double unc = 0.0;
    readDouble(refine, "camera_uncertainty_m", unc, okUnc);
    if (okUnc && unc >= 0.0) out.refinedUncertaintyM = unc;

    bool okConf = false;
    double conf = 0.0;
    readDouble(refine, "camera_confidence", conf, okConf);
    if (okConf) out.refinedConfidence = conf;

    out.refinedNeedsManualReview = refine.value("needs_manual_review").toBool(false);
    out.refinedMethod = refine.value("method").toString().trimmed();
    out.refinedSeedSource = refine.value("seed_source").toString().trimmed();
}

void MapTab::applyVehicleGeo(const QJsonObject& artifactsObj, Item& out)
{
    out.hasVehicleGeo = false;
    out.vehicleLat = 0.0;
    out.vehicleLon = 0.0;
    out.vehicleDistanceM = 0.0;
    out.vehicleBearingDeg = 0.0;
    out.vehicleConfidence = 0.0;
    out.vehicleNeedsManualReview = false;
    out.vehicleGeoStatus.clear();
    out.vehicleGeoJsonPath.clear();

    if (artifactsObj.isEmpty()) return;

    out.vehicleGeoJsonPath = artifactsObj.value("vehicle_geo_json_path").toString().trimmed();

    const QJsonObject geo = artifactsObj.value("vehicle_geo").toObject();
    if (geo.isEmpty()) return;

    auto readDouble = [](const QJsonObject& o, const char* key, double& dst, bool& ok) {
        ok = false;
        const QJsonValue v = o.value(QString::fromLatin1(key));
        if (v.isDouble()) {
            dst = v.toDouble();
            ok = qIsFinite(dst);
        } else if (v.isString()) {
            dst = v.toString().toDouble(&ok);
        }
    };

    out.vehicleGeoStatus = geo.value("status").toString().trimmed();

    bool okLat = false;
    bool okLon = false;
    double lat = 0.0;
    double lon = 0.0;
    readDouble(geo, "vehicle_lat", lat, okLat);
    readDouble(geo, "vehicle_lon", lon, okLon);

    if (okLat && okLon && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
        out.hasVehicleGeo = true;
        out.vehicleLat = lat;
        out.vehicleLon = lon;
    }

    bool okDist = false;
    double dist = 0.0;
    readDouble(geo, "vehicle_distance_m", dist, okDist);
    if (okDist && dist >= 0.0) out.vehicleDistanceM = dist;

    bool okBearing = false;
    double bearing = 0.0;
    readDouble(geo, "vehicle_bearing_deg", bearing, okBearing);
    if (okBearing) out.vehicleBearingDeg = bearing;

    bool okConf = false;
    double conf = 0.0;
    readDouble(geo, "vehicle_confidence", conf, okConf);
    if (okConf) out.vehicleConfidence = conf;

    out.vehicleNeedsManualReview = geo.value("needs_manual_review").toBool(false);
    if (!out.hasVehicleGeo) {
        out.vehicleLat = 0.0;
        out.vehicleLon = 0.0;
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
    s << ("georef_file=" + (it.geoRefPath.isEmpty() ? geoRefSidecarPath(it.imagePath) : it.geoRefPath));

    if (it.hasGps) {
        s << QString("exif_gps=%1, %2").arg(it.lat, 0, 'f', 8).arg(it.lon, 0, 'f', 8);
    } else {
        s << "exif_gps=нет координат";
    }

    if (it.hasCameraPoint) {
        s << QString("manual_camera=%1, %2").arg(it.cameraLat, 0, 'f', 8).arg(it.cameraLon, 0, 'f', 8);
    } else {
        s << "manual_camera=нет";
    }

    QString source;
    double displayLat = 0.0;
    double displayLon = 0.0;
    if (itemDisplayCoords(it, displayLat, displayLon, &source)) {
        s << QString("effective_point=%1, %2").arg(displayLat, 0, 'f', 8).arg(displayLon, 0, 'f', 8);
        s << ("effective_source=" + source);
    } else {
        s << "effective_point=нет";
    }

    if (it.hasCameraAzimuth) {
        s << QString("manual_azimuth_deg=%1").arg(it.cameraAzimuthDeg, 0, 'f', 3);
    } else {
        s << "manual_azimuth_deg=нет";
    }

    s << QString("manual_uncertainty_m=%1").arg(it.uncertaintyM, 0, 'f', 3);

    if (it.hasRefinedCamera) {
        s << QString("camera_refined=%1, %2").arg(it.refinedLat, 0, 'f', 8).arg(it.refinedLon, 0, 'f', 8);
    } else {
        s << "camera_refined=нет";
    }

    if (it.hasRefinedAzimuth) {
        s << QString("camera_refined_azimuth_deg=%1").arg(it.refinedAzimuthDeg, 0, 'f', 3);
    } else {
        s << "camera_refined_azimuth_deg=нет";
    }

    s << QString("camera_uncertainty_m=%1").arg(it.refinedUncertaintyM, 0, 'f', 3);
    s << QString("camera_confidence=%1").arg(it.refinedConfidence, 0, 'f', 6);
    s << QString("camera_needs_manual_review=%1").arg(it.refinedNeedsManualReview ? "true" : "false");

    if (!it.refinedMethod.isEmpty()) s << ("camera_refine_method=" + it.refinedMethod);
    if (!it.refinedSeedSource.isEmpty()) s << ("camera_refine_seed=" + it.refinedSeedSource);
    if (!it.cameraRefineJsonPath.isEmpty()) s << ("camera_refine_json=" + it.cameraRefineJsonPath);

    if (it.hasVehicleGeo) {
        s << QString("vehicle_geo=%1, %2").arg(it.vehicleLat, 0, 'f', 8).arg(it.vehicleLon, 0, 'f', 8);
    } else {
        s << "vehicle_geo=нет";
    }

    s << QString("vehicle_distance_m=%1").arg(it.vehicleDistanceM, 0, 'f', 3);
    s << QString("vehicle_bearing_deg=%1").arg(it.vehicleBearingDeg, 0, 'f', 3);
    s << QString("vehicle_confidence=%1").arg(it.vehicleConfidence, 0, 'f', 6);
    s << QString("vehicle_needs_manual_review=%1").arg(it.vehicleNeedsManualReview ? "true" : "false");

    if (!it.vehicleGeoStatus.isEmpty()) s << ("vehicle_status=" + it.vehicleGeoStatus);
    if (!it.vehicleGeoJsonPath.isEmpty()) s << ("vehicle_geo_json=" + it.vehicleGeoJsonPath);

    if (!it.make.isEmpty()) s << ("make=" + it.make);
    if (!it.model.isEmpty()) s << ("model=" + it.model);
    if (!it.dateTime.isEmpty()) s << ("datetime=" + it.dateTime);
    if (!it.dateTimeOriginal.isEmpty()) s << ("datetime_original=" + it.dateTimeOriginal);

    if (!it.exif.isEmpty()) {
        const QJsonObject data = it.exif.value("data").toObject();

        auto addNum = [&](const char* key, const char* outKey) {
            const QJsonValue v = data.value(QString::fromLatin1(key));
            if (v.isDouble()) {
                s << (QString::fromLatin1(outKey) + "=" + QString::number(v.toDouble(), 'f', 6));
            } else if (v.isString()) {
                const QString t = v.toString().trimmed();
                if (!t.isEmpty()) s << (QString::fromLatin1(outKey) + "=" + t);
            }
        };

        addNum("FNumber", "fnumber");
        addNum("ExposureTime", "exposure");
        addNum("FocalLength", "focal");
        addNum("ISOSpeedRatings", "iso");
    }

    if (it.hasResult) {
        s << "---- result ----";
        s << ("module_id=" + it.result.moduleId);
        s << ("device=" + it.result.deviceUsed);
        s << ("detections=" + QString::number(it.result.detections.size()));

        const QJsonValue distVal = it.result.artifacts.value(QStringLiteral("dist_m"));
        if (distVal.isDouble()) {
            s << QString("dist_m=%1").arg(distVal.toDouble(), 0, 'f', 3);
        } else if (distVal.isString()) {
            s << ("dist_m=" + distVal.toString());
        }
    }

    m_info->setPlainText(s.join("\n"));
}

void MapTab::onMarkerClicked(const QString& imagePath)
{
    selectItem(imagePath);
}

void MapTab::onMapClicked(double lat, double lon)
{
    if (m_selected.isEmpty()) return;

    auto it = m_items.find(m_selected);
    if (it == m_items.end()) return;

    if (!qIsFinite(lat) || !qIsFinite(lon) || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        updateGeoStatus("Координаты карты некорректны.");
        return;
    }

    if (m_editMode == EditMode::SetCameraPoint) {
        it->hasCameraPoint = true;
        it->cameraLat = lat;
        it->cameraLon = lon;
        if (it->uncertaintyM <= 0.0) it->uncertaintyM = 15.0;
        if (it->locationSource.isEmpty() || it->locationSource == QStringLiteral("exif"))
            it->locationSource = QStringLiteral("manual");

        m_editMode = EditMode::Idle;
        updateInfoPanel(*it);
        updateGeoControlsFromSelection();
        pushModelToQml();
        syncSelectedToQml();
        updateGeoStatus("Положение камеры задано. Сохраните геопривязку кнопкой «Сохранить».");
        return;
    }

    if (m_editMode == EditMode::SetCameraDirection) {
        double baseLat = 0.0;
        double baseLon = 0.0;
        if (!itemDisplayCoords(*it, baseLat, baseLon, nullptr)) {
            updateGeoStatus("Сначала задайте точку камеры.");
            m_editMode = EditMode::Idle;
            updateGeoControlsFromSelection();
            return;
        }

        it->hasCameraAzimuth = true;
        it->cameraAzimuthDeg = azimuthDegrees(baseLat, baseLon, lat, lon);

        m_editMode = EditMode::Idle;
        updateInfoPanel(*it);
        updateGeoControlsFromSelection();
        syncSelectedToQml();
        updateGeoStatus("Направление камеры задано. Сохраните геопривязку кнопкой «Сохранить».");
    }
}

void MapTab::onSetCameraPointMode()
{
    if (m_selected.isEmpty()) return;
    m_editMode = EditMode::SetCameraPoint;
    updateGeoStatus();
}

void MapTab::onSetDirectionMode()
{
    if (m_selected.isEmpty()) return;

    auto it = m_items.find(m_selected);
    if (it == m_items.end()) return;

    double lat = 0.0;
    double lon = 0.0;
    if (!itemDisplayCoords(*it, lat, lon, nullptr)) {
        updateGeoStatus("Сначала задайте точку камеры или загрузите EXIF-координаты.");
        return;
    }

    m_editMode = EditMode::SetCameraDirection;
    updateGeoStatus();
}

void MapTab::onClearGeoRef()
{
    if (m_selected.isEmpty()) return;

    auto it = m_items.find(m_selected);
    if (it == m_items.end()) return;

    it->hasCameraPoint = false;
    it->cameraLat = 0.0;
    it->cameraLon = 0.0;
    it->hasCameraAzimuth = false;
    it->cameraAzimuthDeg = 0.0;
    it->locationSource = it->hasGps ? QStringLiteral("exif") : QString();

    m_editMode = EditMode::Idle;
    updateInfoPanel(*it);
    updateGeoControlsFromSelection();
    pushModelToQml();
    syncSelectedToQml();
    updateGeoStatus("Ручная геопривязка очищена. Нажмите «Сохранить», чтобы обновить sidecar-файл.");
}

void MapTab::onSaveGeoRef()
{
    if (m_selected.isEmpty()) return;

    auto it = m_items.find(m_selected);
    if (it == m_items.end()) return;

    it->geoRefPath = geoRefSidecarPath(it->imagePath);
    QString err;
    if (!saveGeoRef(*it, &err)) {
        updateGeoStatus(err.isEmpty() ? QStringLiteral("Не удалось сохранить georef.") : err);
        return;
    }

    updateInfoPanel(*it);
    updateGeoStatus(QStringLiteral("Геопривязка сохранена: ") + it->geoRefPath);
}

void MapTab::onLocationSourceChanged()
{
    if (m_updatingControls || m_selected.isEmpty()) return;

    auto it = m_items.find(m_selected);
    if (it == m_items.end()) return;

    const QString source = m_locationSource->currentData().toString().trimmed();
    if (source.isEmpty()) return;

    it->locationSource = source;
    updateInfoPanel(*it);
    pushModelToQml();
    syncSelectedToQml();
    updateGeoStatus("Источник геопривязки изменён. Сохраните sidecar-файл.");
}

void MapTab::onUncertaintyChanged(double value)
{
    if (m_updatingControls || m_selected.isEmpty()) return;

    auto it = m_items.find(m_selected);
    if (it == m_items.end()) return;

    it->uncertaintyM = value;
    updateInfoPanel(*it);
    syncSelectedToQml();
    updateGeoStatus("Радиус неопределённости изменён. Сохраните sidecar-файл.");
}

void MapTab::onImageSelected(const QString& imagePath)
{
    const QString p = imagePath.trimmed();
    if (p.isEmpty()) return;

    QFileInfo fi(p);
    if (!fi.exists() || !fi.isFile()) return;

    Item it;
    it.imagePath = fi.absoluteFilePath();
    it.geoRefPath = geoRefSidecarPath(it.imagePath);

    // 1) быстрый JPEG-EXIF
    readExifMini(it.imagePath, it);

    // 2) fallback: python preview EXIF (для HEIC/HEIF и др.)
    if (!it.hasGps) {
        QJsonObject exifObj;
        QString err;
        if (readExifViaRunnerPreviewMap(m_cfg, it.imagePath, exifObj, err)) {
            applyRunnerExif(exifObj, it);
        }
    }

    // 3) подгружаем sidecar геопривязки, если есть
    QString geoErr;
    loadGeoRef(it.imagePath, it, &geoErr);
    if (it.locationSource.isEmpty())
        it.locationSource = defaultLocationSource(it);

    m_items.insert(it.imagePath, it);
    pushModelToQml();
    selectItem(it.imagePath);
}

void MapTab::onResultReady(const QString& imagePath, const ModuleResult& r)
{
    const QString p = imagePath.trimmed();
    if (p.isEmpty()) return;

    const QString ap = QFileInfo(p).absoluteFilePath();

    auto it = m_items.find(ap);
    if (it == m_items.end()) {
        Item ni;
        ni.imagePath = ap;
        ni.geoRefPath = geoRefSidecarPath(ap);
        readExifMini(ni.imagePath, ni);

        QString geoErr;
        loadGeoRef(ni.imagePath, ni, &geoErr);
        ni.locationSource = defaultLocationSource(ni);

        it = m_items.insert(ap, ni);
    }

    it->hasResult = true;
    it->result = r;

    if (!r.exif.isEmpty()) {
        applyRunnerExif(r.exif, *it);
        if (!it->hasCameraPoint && it->locationSource.isEmpty())
            it->locationSource = defaultLocationSource(*it);
    }

    applyCameraRefine(r.artifacts, *it);
    applyVehicleGeo(r.artifacts, *it);
    pushModelToQml();
    syncSelectedToQml();

    if (m_selected == ap)
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
