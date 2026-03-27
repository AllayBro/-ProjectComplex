#include "MapTab.h"

#include <QApplication>
#include <QClipboard>
#include <QShortcut>
#include <QItemSelectionModel>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QFile>
#include <QDir>
#include <QFrame>
#include <QtGlobal>
#include <QtQuickWidgets/QQuickWidget>
#include <QtQuick/QQuickItem>
#include <QtQml/QQmlEngine>
#include <QMetaObject>
#include <QCoreApplication>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPushButton>
#include <QSignalBlocker>
#include <QtMath>
#include <cmath>
#include <algorithm>
#include <limits>
#include <QItemSelectionModel>


static QVariantMap vehicleShapeLocal(double lat,
                                     double lon,
                                     double headingDeg,
                                     double lengthM,
                                     double widthM,
                                     const QString& label = QString());

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

    property var vehiclesModel: []
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
            anchorPoint.x: markerBody.width / 2
            anchorPoint.y: markerBody.height / 2

            sourceItem: Rectangle {
                id: markerBody
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
        id: activeCameraMarkerComponent
        Item {
            width: 32
            height: 32

            Item {
                anchors.centerIn: parent
                width: 32
                height: 32
                rotation: root.hasSelectedDirection ? root.selectedAzimuthDeg : 0.0
                transformOrigin: Item.Center

                Rectangle {
                    x: 10
                    y: 10
                    width: 12
                    height: 12
                    radius: 6
                    color: "#1976d2"
                    border.color: "white"
                    border.width: 2
                }

                Rectangle {
                    visible: root.hasSelectedDirection
                    x: 14
                    y: 1
                    width: 4
                    height: 10
                    radius: 2
                    color: "#43a047"
                    border.color: "white"
                    border.width: 1
                }
            }
        }
    }
    Component {
        id: vehicleDelegate
        MapQuickItem {
            coordinate: QtPositioning.coordinate(modelData.lat, modelData.lon)
            anchorPoint.x: 18
            anchorPoint.y: 10

            sourceItem: Item {
                width: 72
                height: modelData.label && modelData.label.length > 0 ? 34 : 24

                Item {
                    x: 8
                    y: 4
                    width: 40
                    height: 16
                    rotation: modelData.headingDeg || 0.0
                    transformOrigin: Item.Center

                    Rectangle {
                        anchors.centerIn: parent
                        width: 28
                        height: 12
                        radius: 2
                        color: "#8e24aa"
                        border.color: "white"
                        border.width: 2
                    }

                    Rectangle {
                        x: 24
                        y: 4
                        width: 10
                        height: 4
                        radius: 1
                        color: "#ce93d8"
                        border.color: "white"
                        border.width: 1
                    }
                }

                Rectangle {
                    visible: modelData.label && modelData.label.length > 0
                    x: 2
                    y: 22
                    color: "#ffffff"
                    opacity: 0.9
                    radius: 4
                    border.color: "#8e24aa"
                    border.width: 1

                    Text {
                        anchors.margins: 4
                        anchors.fill: parent
                        text: modelData.label
                        color: "#202020"
                        font.pixelSize: 11
                        wrapMode: Text.NoWrap
                    }

                    width: textItem.implicitWidth + 8
                    height: textItem.implicitHeight + 6

                    Text {
                        id: textItem
                        visible: false
                        text: modelData.label
                        font.pixelSize: 11
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
            anchorPoint.x: 6
            anchorPoint.y: 6
            sourceItem: Rectangle {
                width: 12
                height: 12
                radius: 6
                color: "#e53935"
                opacity: 0.35
                border.color: "white"
                border.width: 1
            }
        }

        MapQuickItem {
            visible: root.hasSelectedPoint
            z: 1000
            coordinate: QtPositioning.coordinate(root.selectedLat, root.selectedLon)
            anchorPoint.x: 16
            anchorPoint.y: 16
            sourceItem: Loader {
                width: 32
                height: 32
                sourceComponent: activeCameraMarkerComponent
            }
        }

        MapItemView { model: root.vehiclesModel; delegate: vehicleDelegate }


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
            anchorPoint.x: 6
            anchorPoint.y: 6
            sourceItem: Rectangle {
                width: 12
                height: 12
                radius: 6
                color: "#e53935"
                opacity: 0.35
                border.color: "white"
                border.width: 1
            }
        }

        MapQuickItem {
            visible: root.hasSelectedPoint
            coordinate: QtPositioning.coordinate(root.selectedLat, root.selectedLon)
            anchorPoint.x: 16
            anchorPoint.y: 16
            sourceItem: activeCameraMarkerComponent
        }

        MapItemView { model: root.vehiclesModel; delegate: vehicleDelegate }


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

QString MapTab::normalizedImageKey(const QString& imagePath)
{
    QFileInfo fi(imagePath);

    QString p = fi.canonicalFilePath();
    if (p.isEmpty()) p = fi.absoluteFilePath();

    p = QDir::cleanPath(QDir::fromNativeSeparators(p));

#ifdef Q_OS_WIN
    p = p.toLower();
#endif

    return p;
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


static QString boolMarkLocal(bool value)
{
    return value ? QStringLiteral("да") : QStringLiteral("нет");
}

static QString coordTextLocal(bool hasPoint, double lat, double lon)
{
    if (!hasPoint)
        return QStringLiteral("—");
    return QString::number(lat, 'f', 7) + QStringLiteral(", ") + QString::number(lon, 'f', 7);
}

static QString locationSourceTextLocal(bool hasCameraPoint, bool hasRefinedCamera, bool hasCoarseGeo, bool hasGps)
{
    if (hasCameraPoint)
        return QStringLiteral("manual");
    if (hasRefinedCamera)
        return QStringLiteral("refined");
    if (hasCoarseGeo)
        return QStringLiteral("coarse");
    if (hasGps)
        return QStringLiteral("exif");
    return QStringLiteral("—");
}

static QString shortNameLocal(const QString& path)
{
    return QFileInfo(path).fileName();
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
    geoRow1->addWidget(m_btnSetCameraPoint);
    geoRow1->addWidget(m_btnSetDirection);
    geoRow1->addWidget(m_btnClearGeoRef);
    rlay->addLayout(geoRow1);

    m_geoStatus = new QLabel(this);
    m_geoStatus->setWordWrap(true);
    m_geoStatus->setMinimumWidth(260);
    rlay->addWidget(m_geoStatus);

    auto* imagesTitle = new QLabel("Фотографии", this);
    rlay->addWidget(imagesTitle);

    m_imagesTable = new QTableWidget(this);
    m_imagesTable->setColumnCount(5);
    m_imagesTable->setHorizontalHeaderLabels(QStringList{QStringLiteral("Фото"), QStringLiteral("Источник"), QStringLiteral("EXIF"), QStringLiteral("Ручн."), QStringLiteral("Результат")});
    m_imagesTable->horizontalHeader()->setStretchLastSection(false);
    m_imagesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_imagesTable->horizontalHeader()->setStretchLastSection(false);
    m_imagesTable->resizeColumnsToContents();
    m_imagesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_imagesTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_imagesTable->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_imagesTable->setAlternatingRowColors(true);
    m_imagesTable->setShowGrid(true);
    m_imagesTable->setGridStyle(Qt::SolidLine);
    m_imagesTable->setMinimumHeight(180);
    m_imagesTable->setStyleSheet(QString());
    rlay->addWidget(m_imagesTable, 1);

    auto* copyShortcut = new QShortcut(QKeySequence::Copy, m_imagesTable);
    connect(copyShortcut, &QShortcut::activated, this, [this]() {
        if (!m_imagesTable) return;

        auto* sm = m_imagesTable->selectionModel();
        if (!sm) return;

        QModelIndexList rows = sm->selectedRows();
        if (rows.isEmpty()) return;

        std::sort(rows.begin(), rows.end(),
                  [](const QModelIndex& a, const QModelIndex& b) {
                      return a.row() < b.row();
                  });

        QStringList lines;
        lines << QStringLiteral("Фото\tИсточник\tEXIF\tРучн.\tРезультат");

        for (const QModelIndex& idx : rows) {
            const int row = idx.row();
            QStringList cells;
            for (int col = 0; col < m_imagesTable->columnCount(); ++col) {
                QTableWidgetItem* item = m_imagesTable->item(row, col);
                cells << (item ? item->text() : QString());
            }
            lines << cells.join('\t');
        }

        QApplication::clipboard()->setText(lines.join('\n'));
    });

    auto* infoTitle = new QLabel("Информация по выбранной фотографии", this);
    rlay->addWidget(infoTitle);

    m_infoPanel = new QLabel(this);
    m_infoPanel->setWordWrap(true);
    m_infoPanel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_infoPanel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_infoPanel->setFrameShape(QFrame::StyledPanel);
    m_infoPanel->setMinimumHeight(220);
    rlay->addWidget(m_infoPanel);
    split->addWidget(m_quick);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    root->addWidget(split, 1);

    connect(m_btnSetCameraPoint, &QPushButton::clicked, this, &MapTab::onSetCameraPointMode);
    connect(m_btnSetDirection, &QPushButton::clicked, this, &MapTab::onSetDirectionMode);
    connect(m_btnClearGeoRef, &QPushButton::clicked, this, &MapTab::onClearGeoRef);
    connect(m_imagesTable->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection&, const QItemSelection&) {
        if (!m_imagesTable) return;

        const QModelIndex current = m_imagesTable->currentIndex();
        if (!current.isValid()) return;

        QTableWidgetItem* item = m_imagesTable->item(current.row(), 0);
        if (!item) return;

        const QString key = item->data(Qt::UserRole).toString();
        if (key.isEmpty()) return;

        auto it = m_items.find(key);
        if (it == m_items.end()) return;

        m_selected = key;
        pushModelToQml();
        syncSelectedToQml();
        updateGeoControlsFromSelection();
        updateInfoPanel(*it);
    });
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
        updateGeoStatus("QML Error: " + errs.join(" | "));
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
                updateGeoStatus("Async QML Error: " + errs.join(" | "));
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
    if (it.hasCameraPoint) {
        lat = it.cameraLat;
        lon = it.cameraLon;
        if (source) *source = QStringLiteral("manual");
        return true;
    }

    if (it.hasRefinedCamera) {
        lat = it.refinedLat;
        lon = it.refinedLon;
        if (source) *source = QStringLiteral("refined");
        return true;
    }

    if (it.hasCoarseGeo) {
        lat = it.coarseLat;
        lon = it.coarseLon;
        if (source) *source = QStringLiteral("coarse");
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
    if (it.hasCameraPoint) return QStringLiteral("manual");
    if (it.hasRefinedCamera) return QStringLiteral("refined");
    if (it.hasGps) return QStringLiteral("exif");
    return QStringLiteral("estimated");
}

bool MapTab::isLocalExifRefine(const Item& it)
{
    if (!it.hasRefinedCamera)
        return false;

    const QString seed = it.refinedSeedSource.trimmed().toLower();
    const QString method = it.refinedMethod.trimmed().toLower();

    return seed == QStringLiteral("exif")
           && method.contains(QStringLiteral("seed_window"));
}

QString MapTab::autoSidecarLocationSource(const Item& it)
{
    if (it.hasCameraPoint)
        return QStringLiteral("manual");

    if (it.hasGps)
        return QStringLiteral("exif");

    return QStringLiteral("estimated");
}

double MapTab::autoSidecarUncertaintyM(const Item& it)
{
    if (it.hasRefinedCamera && it.refinedUncertaintyM > 0.0)
        return it.refinedUncertaintyM;

    if (it.uncertaintyM > 0.0)
        return it.uncertaintyM;

    return 15.0;
}

bool MapTab::isManualReviewRecommended(const Item& it)
{
    if (it.refinedNeedsManualReview || it.vehicleNeedsManualReview)
        return true;

    if (it.hasRefinedCamera && !it.hasRefinedAzimuth && !it.hasCameraAzimuth)
        return true;

    if (!it.hasRefinedCamera && it.hasGps && !it.hasCameraPoint)
        return true;

    return false;
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

    const QString autoSource = autoSidecarLocationSource(it);
    const double autoUncertainty = autoSidecarUncertaintyM(it);

    QJsonObject o;
    o.insert(QStringLiteral("image_path"), it.imagePath);
    if (it.hasCameraPoint) {
        o.insert(QStringLiteral("camera_lat"), it.cameraLat);
        o.insert(QStringLiteral("camera_lon"), it.cameraLon);
    }
    if (it.hasCameraAzimuth) {
        o.insert(QStringLiteral("camera_azimuth_deg"), it.cameraAzimuthDeg);
    }
    o.insert(QStringLiteral("location_source"), autoSource);
    o.insert(QStringLiteral("uncertainty_m"), autoUncertainty);
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
    ro->setProperty("vehiclesModel", QVariantList{});

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

    QVariantList vehicles = it.vehicleShapes;
    if (vehicles.isEmpty())
        vehicles = it.vehiclePoints;

    if (vehicles.isEmpty() && it.hasVehicleGeo) {
        vehicles.push_back(vehicleShapeLocal(it.vehicleLat, it.vehicleLon, it.vehicleBearingDeg, 4.8, 1.9));
    }

    ro->setProperty("vehiclesModel", vehicles);

    double lat = 0.0;
    double lon = 0.0;
    if (!itemDisplayCoords(it, lat, lon, nullptr)) return;

    ro->setProperty("hasSelectedPoint", true);
    ro->setProperty("selectedLat", lat);
    ro->setProperty("selectedLon", lon);

    bool hasDirection = false;
    double azimuth = 0.0;
    double unc = it.uncertaintyM;

    if (it.hasCameraPoint) {
        if (it.hasCameraAzimuth) {
            hasDirection = true;
            azimuth = it.cameraAzimuthDeg;
        } else if (it.hasRefinedAzimuth) {
            hasDirection = true;
            azimuth = it.refinedAzimuthDeg;
        }
        unc = it.uncertaintyM;
    } else if (it.hasRefinedCamera) {
        hasDirection = it.hasRefinedAzimuth || it.hasCameraAzimuth;
        azimuth = it.hasRefinedAzimuth ? it.refinedAzimuthDeg : it.cameraAzimuthDeg;
        unc = it.refinedUncertaintyM > 0.0 ? it.refinedUncertaintyM : it.uncertaintyM;
    } else {
        hasDirection = it.hasCameraAzimuth;
        azimuth = it.cameraAzimuthDeg;
        unc = it.uncertaintyM;
    }

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
        return;
    case EditMode::SetCameraDirection:
        m_geoStatus->setText("Режим: щёлкните по карте второй точкой, чтобы задать азимут камеры.");
        return;
    case EditMode::Idle:
    default:
        break;
    }

    if (m_selected.isEmpty() || !m_items.contains(m_selected)) {
        m_geoStatus->setText("Снимок не выбран.");
        return;
    }

    const Item& it = m_items[m_selected];

    if (it.hasCameraPoint) {
        if (it.hasCameraAzimuth) {
            m_geoStatus->setText("Точка и направление камеры заданы вручную.");
        } else {
            m_geoStatus->setText("Точка камеры задана вручную. Теперь укажите направление кнопкой «Направление».");
        }
        return;
    }

    const bool hasDirection = it.hasRefinedAzimuth || it.hasCameraAzimuth;
    const bool recommendReview = isManualReviewRecommended(it);

    if (it.hasRefinedCamera) {
        if (isLocalExifRefine(it)) {
            if (hasDirection) {
                m_geoStatus->setText(
                    recommendReview
                        ? "Локальное уточнение выполнено рядом с EXIF-подсказкой. Рекомендуется ручная проверка."
                        : "Локальное уточнение выполнено рядом с EXIF-подсказкой."
                );
            } else {
                m_geoStatus->setText("Локальное уточнение выполнено рядом с EXIF-подсказкой, но направление не подтверждено. Рекомендуется ручная проверка.");
            }
        } else {
            if (hasDirection) {
                m_geoStatus->setText(
                    recommendReview
                        ? "Точка и направление найдены автоматически. Рекомендуется ручная проверка."
                        : "Точка и направление определены автоматически."
                );
            } else {
                m_geoStatus->setText("Точка камеры найдена автоматически, но направление не подтверждено. Рекомендуется ручная проверка.");
            }
        }
        return;
    }

    if (it.hasGps) {
        m_geoStatus->setText("Используются EXIF-координаты как подсказка. Точное положение ещё не подтверждено.");
        return;
    }

    m_geoStatus->setText("Точка камеры не определена.");
}

void MapTab::updateGeoControlsFromSelection()
{
    const bool hasSelection = !m_selected.isEmpty() && m_items.contains(m_selected);

    m_btnSetCameraPoint->setEnabled(hasSelection);

    bool canSetDirection = false;
    bool canClearManualGeo = false;
    if (hasSelection) {
        const Item& it = m_items[m_selected];
        canSetDirection = it.hasCameraPoint;
        canClearManualGeo = it.hasCameraPoint || it.hasCameraAzimuth;
    }

    m_btnSetDirection->setEnabled(canSetDirection);
    m_btnClearGeoRef->setEnabled(canClearManualGeo);

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
        m.insert("selected", normalizedImageKey(v.imagePath) == m_selected);
        list.push_back(m);
    }

    ro->setProperty("pointsModel", list);
    refreshItemsTable();
}

void MapTab::selectItem(const QString& imagePath)
{
    const QString key = normalizedImageKey(imagePath);

    auto it = m_items.find(key);
    if (it == m_items.end()) return;

    m_selected = key;

    const Item& v = it.value();

    pushModelToQml();
    syncSelectedToQml();
    updateGeoControlsFromSelection();
    updateInfoPanel(v);

    double mapLat = 0.0;
    double mapLon = 0.0;
    bool hasPoint = false;

    if (itemDisplayCoords(v, mapLat, mapLon, nullptr)) {
        hasPoint = true;
    } else if (v.hasVehicleGeo) {
        mapLat = v.vehicleLat;
        mapLon = v.vehicleLon;
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

void MapTab::applyCoarseGeoSearch(const QJsonObject& artifactsObj, Item& out)
{
    out.hasCoarseGeo = false;
    out.coarseLat = 0.0;
    out.coarseLon = 0.0;
    out.coarseRadiusM = 0.0;
    out.coarseConfidence = 0.0;
    out.coarseMethod.clear();
    out.coarseSeedSource.clear();
    out.coarseGeoJsonPath.clear();

    if (artifactsObj.isEmpty())
        return;

    out.coarseGeoJsonPath = artifactsObj.value("coarse_geo_json_path").toString().trimmed();

    const QJsonObject coarse = artifactsObj.value("coarse_geo_search").toObject();
    if (coarse.isEmpty())
        return;

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

    readDouble(coarse, "best_lat", lat, okLat);
    readDouble(coarse, "best_lon", lon, okLon);

    if (okLat && okLon && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
        out.hasCoarseGeo = true;
        out.coarseLat = lat;
        out.coarseLon = lon;
    }

    bool okRadius = false;
    double radius = 0.0;
    readDouble(coarse, "search_radius_m", radius, okRadius);
    if (okRadius && radius >= 0.0)
        out.coarseRadiusM = radius;

    bool okConf = false;
    double conf = 0.0;
    readDouble(coarse, "confidence", conf, okConf);
    if (okConf)
        out.coarseConfidence = conf;

    out.coarseMethod = coarse.value("method").toString().trimmed();
    out.coarseSeedSource = coarse.value("seed_source").toString().trimmed();
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
    out.refinedMethod = refine.value("refine_method").toString().trimmed();
    if (out.refinedMethod.isEmpty())
        out.refinedMethod = refine.value("method").toString().trimmed();

    out.refinedSeedSource = refine.value("seed_source").toString().trimmed();
}
void MapTab::applyVehicleGeo(const QJsonObject& artifactsObj, Item& out)
{
    clearVehicleGeo(out);
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

    const QJsonArray vehicles = geo.value("vehicles").toArray();
    for (int i = 0; i < vehicles.size(); ++i)
    {
        const QJsonObject v = vehicles.at(i).toObject();
        if (v.isEmpty()) continue;

        bool okVLat = false;
        bool okVLon = false;
        double vLat = 0.0;
        double vLon = 0.0;
        readDouble(v, "vehicle_lat", vLat, okVLat);
        readDouble(v, "vehicle_lon", vLon, okVLon);

        if (!(okVLat && okVLon && vLat >= -90.0 && vLat <= 90.0 && vLon >= -180.0 && vLon <= 180.0))
            continue;

        double headingDeg = out.vehicleBearingDeg;
        bool okHeading = false;
        readDouble(v, "vehicle_bearing_deg", headingDeg, okHeading);
        if (!okHeading)
            readDouble(v, "bearing_deg", headingDeg, okHeading);
        if (!okHeading)
            headingDeg = out.vehicleBearingDeg;

        double lengthM = 4.8;
        bool okLength = false;
        readDouble(v, "length_m", lengthM, okLength);

        double widthM = 1.9;
        bool okWidth = false;
        readDouble(v, "width_m", widthM, okWidth);

        QString label;
        const double distForLabel = v.value("vehicle_distance_m").toDouble(v.value("distance_m").toDouble(-1.0));
        if (distForLabel > 0.0)
            label = QString::number(distForLabel, 'f', 1) + QStringLiteral(" м");

        out.vehicleShapes.push_back(vehicleShapeLocal(vLat, vLon, headingDeg, lengthM, widthM, label));
    }

    if (out.vehiclePoints.isEmpty() && out.hasVehicleGeo) {
        QVariantMap point;
        point.insert("lat", out.vehicleLat);
        point.insert("lon", out.vehicleLon);
        out.vehiclePoints.push_back(point);
        out.vehicleShapes.push_back(vehicleShapeLocal(out.vehicleLat, out.vehicleLon, out.vehicleBearingDeg, 4.8, 1.9));
    }

    if (!out.hasVehicleGeo) {
        out.vehicleLat = 0.0;
        out.vehicleLon = 0.0;
    }
}

static bool readJsonDoubleFlexibleLocal(const QJsonObject& o, const char* key, double& out)
{
    bool ok = false;
    const QJsonValue v = o.value(QString::fromLatin1(key));


    if (v.isDouble()) {
        out = v.toDouble();
        ok = qIsFinite(out);
    } else if (v.isString()) {
        out = v.toString().toDouble(&ok);
        ok = ok && qIsFinite(out);
    }

    return ok;
}

static bool detectionDistanceMetersLocal(const Detection& det, double& out)
{
    if (readJsonDoubleFlexibleLocal(det.meta, "dist_m", out) && out > 0.0) return true;
    if (readJsonDoubleFlexibleLocal(det.meta, "distance_m", out) && out > 0.0) return true;

    out = 0.0;
    return false;
}

static double normalize360Local(double deg)
{
    while (deg < 0.0) deg += 360.0;
    while (deg >= 360.0) deg -= 360.0;
    return deg;
}

static QPair<double, double> destinationPointLocal(double latDeg, double lonDeg, double bearingDeg, double distanceM)
{
    constexpr double R = 6378137.0;

    const double lat1 = qDegreesToRadians(latDeg);
    const double lon1 = qDegreesToRadians(lonDeg);
    const double brng = qDegreesToRadians(bearingDeg);
    const double ang = distanceM / R;

    const double lat2 = std::asin(
        std::sin(lat1) * std::cos(ang) +
        std::cos(lat1) * std::sin(ang) * std::cos(brng)
    );

    const double lon2 = lon1 + std::atan2(
        std::sin(brng) * std::sin(ang) * std::cos(lat1),
        std::cos(ang) - std::sin(lat1) * std::sin(lat2)
    );

    return qMakePair(qRadiansToDegrees(lat2), qRadiansToDegrees(lon2));
}


static QVariantMap vehicleShapeLocal(double lat,
                                     double lon,
                                     double headingDeg,
                                     double lengthM,
                                     double widthM,
                                     const QString& label)
{
    QVariantMap shape;
    shape.insert("lat", lat);
    shape.insert("lon", lon);
    shape.insert("headingDeg", normalize360Local(headingDeg));
    shape.insert("lengthM", lengthM > 0.1 ? lengthM : 4.8);
    shape.insert("widthM", widthM > 0.1 ? widthM : 1.9);
    if (!label.trimmed().isEmpty())
        shape.insert("label", label.trimmed());
    return shape;
}

void MapTab::clearVehicleGeo(Item& it)
{
    it.hasVehicleGeo = false;
    it.vehicleLat = 0.0;
    it.vehicleLon = 0.0;
    it.vehicleDistanceM = 0.0;
    it.vehicleBearingDeg = 0.0;
    it.vehicleConfidence = 0.0;
    it.vehicleNeedsManualReview = false;
    it.vehicleGeoStatus.clear();
    it.vehiclePoints.clear();
    it.vehicleShapes.clear();
}

// Full Distance pipeline stores focal length per-detection in meta ("focal_px"), not always at
// artifacts root (focal_px_x/y from EXIF-based notebooks). Python projection falls back to 0°
// lateral offset when root focal is missing; we must still resolve a focal here or manual
// camera re-projection never runs and vehicle markers stay at the pre-refine positions.
static bool resolveFocalPixelsForProjection(const ModuleResult& r, double& focalPxOut)
{
    focalPxOut = 0.0;
    if (readJsonDoubleFlexibleLocal(r.artifacts, "focal_px_x", focalPxOut) && focalPxOut > 1.0)
        return true;
    focalPxOut = 0.0;
    if (readJsonDoubleFlexibleLocal(r.artifacts, "focal_px_y", focalPxOut) && focalPxOut > 1.0)
        return true;

    focalPxOut = 0.0;
    for (const Detection& det : r.detections) {
        double fp = 0.0;
        if (readJsonDoubleFlexibleLocal(det.meta, "focal_px", fp) && fp > 1.0) {
            focalPxOut = fp;
            return true;
        }
    }

    // Align with py_src/prototypes/api.py DistanceEstimator defaults (focal_mm / sensor_width_mm) * w
    if (r.imageW > 0) {
        constexpr double kFocalMm = 4.25;
        constexpr double kSensorWmm = 5.6;
        focalPxOut = (kFocalMm / kSensorWmm) * double(r.imageW);
        return focalPxOut > 1.0;
    }
    return false;
}


static bool detectionBottomCenterLocal(const Detection& det, double& xOut, double& yOut)
{
    const QJsonValue bcValue = det.meta.value(QStringLiteral("bottom_center_px"));
    if (bcValue.isArray()) {
        const QJsonArray arr = bcValue.toArray();
        if (arr.size() >= 2) {
            const double x = arr.at(0).toDouble(std::numeric_limits<double>::quiet_NaN());
            const double y = arr.at(1).toDouble(std::numeric_limits<double>::quiet_NaN());
            if (qIsFinite(x) && qIsFinite(y)) {
                xOut = x;
                yOut = y;
                return true;
            }
        }
    }

    if (det.x2 <= det.x1 || det.y2 <= det.y1)
        return false;

    xOut = 0.5 * (double(det.x1) + double(det.x2));
    yOut = double(det.y2);
    return qIsFinite(xOut) && qIsFinite(yOut);
}

static double detectionHeadingOffsetDegLocal(const Detection& det, const ModuleResult& r, double focalPx)
{
    if (r.imageW <= 0 || focalPx <= 1.0)
        return 0.0;

    double x = 0.0;
    double y = 0.0;
    if (!detectionBottomCenterLocal(det, x, y))
        return 0.0;

    const double dx = x - 0.5 * double(r.imageW);
    const double offset = qRadiansToDegrees(std::atan2(dx, focalPx));
    return std::clamp(offset, -30.0, 30.0);
}

static void detectionVehicleSizeLocal(const Detection& det, double& lengthM, double& widthM)
{
    lengthM = 4.8;
    widthM = 1.9;

    double value = 0.0;
    if (readJsonDoubleFlexibleLocal(det.meta, "length_m", value) && value > 0.1)
        lengthM = value;
    value = 0.0;
    if (readJsonDoubleFlexibleLocal(det.meta, "width_m", value) && value > 0.1)
        widthM = value;
}

static bool readPreferredVehicleSourceIndexLocal(const ModuleResult& r, int& out)
{
    out = -1;
    const QJsonObject geo = r.artifacts.value(QStringLiteral("vehicle_geo")).toObject();
    if (geo.isEmpty())
        return false;

    const QJsonValue v = geo.value(QStringLiteral("source_detection_index"));
    if (!v.isDouble())
        return false;

    out = v.toInt(-1);
    return out >= 0;
}

static bool readBestCandidateHeadingLocal(const ModuleResult& r, double& headingDeg)
{
    headingDeg = 0.0;
    const QJsonObject refine = r.artifacts.value(QStringLiteral("camera_refine")).toObject();
    if (refine.isEmpty())
        return false;

    if (readJsonDoubleFlexibleLocal(refine, "camera_refined_azimuth_deg", headingDeg))
        return true;

    const QJsonObject candidateSearch = refine.value(QStringLiteral("candidate_search")).toObject();
    if (candidateSearch.isEmpty())
        return false;

    const QJsonObject bestCandidate = candidateSearch.value(QStringLiteral("best_candidate")).toObject();
    if (bestCandidate.isEmpty())
        return false;

    return readJsonDoubleFlexibleLocal(bestCandidate, "heading_deg", headingDeg);
}

bool MapTab::reprojectVehiclesForCurrentCamera(Item& it, QString* err)
{
    if (err) err->clear();

    if (!it.hasResult)
        return false;

    if (!it.hasCameraPoint) {
        if (err) *err = QStringLiteral("Сначала задайте точку камеры.");
        return false;
    }

    double cameraAzimuthDeg = 0.0;
    bool hasAzimuth = false;

    if (it.hasCameraAzimuth) {
        cameraAzimuthDeg = normalizeAzimuth(it.cameraAzimuthDeg);
        hasAzimuth = true;
    } else if (it.hasRefinedAzimuth) {
        cameraAzimuthDeg = normalizeAzimuth(it.refinedAzimuthDeg);
        hasAzimuth = true;
    } else if (readBestCandidateHeadingLocal(it.result, cameraAzimuthDeg)) {
        cameraAzimuthDeg = normalizeAzimuth(cameraAzimuthDeg);
        hasAzimuth = true;
    }

    if (!hasAzimuth) {
        if (err) *err = QStringLiteral("Для пересчёта машин требуется направление камеры.");
        return false;
    }

    double focalPx = 0.0;
    if (!resolveFocalPixelsForProjection(it.result, focalPx) || focalPx <= 1.0) {
        if (err) *err = QStringLiteral("Не удалось определить фокусное расстояние для пересчёта машин.");
        return false;
    }

    QVariantList newVehiclePoints;
    QVariantList newVehicleShapes;

    bool hasPrimaryVehicle = false;
    double primaryLat = 0.0;
    double primaryLon = 0.0;
    double primaryDistanceM = 0.0;
    double primaryBearingDeg = 0.0;
    double primaryConfidence = 0.0;
    int preferredSourceIndex = -1;
    readPreferredVehicleSourceIndexLocal(it.result, preferredSourceIndex);

    for (int detIndex = 0; detIndex < it.result.detections.size(); ++detIndex) {
        const Detection& det = it.result.detections.at(detIndex);

        double distanceM = 0.0;
        if (!detectionDistanceMetersLocal(det, distanceM) || distanceM <= 0.0)
            continue;

        distanceM = std::clamp(distanceM, 1.0, 300.0);

        const double headingOffsetDeg = detectionHeadingOffsetDegLocal(det, it.result, focalPx);
        const double bearingDeg = normalize360Local(cameraAzimuthDeg + headingOffsetDeg);
        const auto latLon = destinationPointLocal(it.cameraLat, it.cameraLon, bearingDeg, distanceM);

        double lengthM = 4.8;
        double widthM = 1.9;
        detectionVehicleSizeLocal(det, lengthM, widthM);

        QVariantMap point;
        point.insert(QStringLiteral("lat"), latLon.first);
        point.insert(QStringLiteral("lon"), latLon.second);
        newVehiclePoints.push_back(point);

        QString label = QString::number(distanceM, 'f', 1) + QStringLiteral(" м");
        newVehicleShapes.push_back(vehicleShapeLocal(latLon.first, latLon.second, bearingDeg, lengthM, widthM, label));

        const double confidence = std::clamp(0.45 + 0.55 * det.conf, 0.0, 1.0);
        const bool useAsPrimary = !hasPrimaryVehicle || detIndex == preferredSourceIndex;
        if (useAsPrimary) {
            hasPrimaryVehicle = true;
            primaryLat = latLon.first;
            primaryLon = latLon.second;
            primaryDistanceM = distanceM;
            primaryBearingDeg = bearingDeg;
            primaryConfidence = confidence;
        }
    }

    if (!hasPrimaryVehicle || newVehicleShapes.isEmpty()) {
        if (err) *err = QStringLiteral("Нет данных для локального пересчёта машин.");
        return false;
    }

    clearVehicleGeo(it);
    it.hasVehicleGeo = true;
    it.vehicleLat = primaryLat;
    it.vehicleLon = primaryLon;
    it.vehicleDistanceM = primaryDistanceM;
    it.vehicleBearingDeg = primaryBearingDeg;
    it.vehicleConfidence = primaryConfidence;
    it.vehicleNeedsManualReview = true;
    it.vehicleGeoStatus = QStringLiteral("manual_reproject_local");
    it.vehiclePoints = newVehiclePoints;
    it.vehicleShapes = newVehicleShapes;
    return true;
}


void MapTab::refreshItemsTable()
{
    if (!m_imagesTable)
        return;

    QVector<QString> keys;
    keys.reserve(m_items.size());
    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it)
        keys.push_back(it.key());

    std::sort(keys.begin(), keys.end(), [this](const QString& a, const QString& b) {
        const Item& ia = m_items[a];
        const Item& ib = m_items[b];
        return QString::localeAwareCompare(shortNameLocal(ia.imagePath), shortNameLocal(ib.imagePath)) < 0;
    });

    QSignalBlocker blocker(m_imagesTable);
    m_imagesTable->setRowCount(keys.size());

    for (int row = 0; row < keys.size(); ++row) {
        const QString& key = keys[row];
        const Item& it = m_items[key];

        auto* nameItem = new QTableWidgetItem(shortNameLocal(it.imagePath));
        nameItem->setData(Qt::UserRole, key);
        nameItem->setToolTip(it.imagePath);
        m_imagesTable->setItem(row, 0, nameItem);
        m_imagesTable->setItem(row, 1, new QTableWidgetItem(locationSourceTextLocal(it.hasCameraPoint, it.hasRefinedCamera, it.hasCoarseGeo, it.hasGps)));
        m_imagesTable->setItem(row, 2, new QTableWidgetItem(boolMarkLocal(it.hasGps)));
        m_imagesTable->setItem(row, 3, new QTableWidgetItem(boolMarkLocal(it.hasCameraPoint || it.hasCameraAzimuth)));
        m_imagesTable->setItem(row, 4, new QTableWidgetItem(boolMarkLocal(it.hasResult)));

    }
}

void MapTab::updateInfoPanel(const Item& it)
{
    if (!m_infoPanel)
        return;

    QStringList lines;
    lines << QStringLiteral("Файл: ") + QFileInfo(it.imagePath).fileName();
    lines << QStringLiteral("Путь: ") + it.imagePath;
    lines << QStringLiteral("Источник точки на карте: ") + locationSourceTextLocal(it.hasCameraPoint, it.hasRefinedCamera, it.hasCoarseGeo, it.hasGps);
    lines << QStringLiteral("EXIF: ") + coordTextLocal(it.hasGps, it.lat, it.lon);
    lines << QStringLiteral("Ручная точка камеры: ") + coordTextLocal(it.hasCameraPoint, it.cameraLat, it.cameraLon);
    lines << QStringLiteral("Ручное направление: ") + (it.hasCameraAzimuth ? QString::number(it.cameraAzimuthDeg, 'f', 1) + QStringLiteral("°") : QStringLiteral("—"));
    lines << QStringLiteral("Уточнённая точка: ") + coordTextLocal(it.hasRefinedCamera, it.refinedLat, it.refinedLon);
    lines << QStringLiteral("Уточнённое направление: ") + (it.hasRefinedAzimuth ? QString::number(it.refinedAzimuthDeg, 'f', 1) + QStringLiteral("°") : QStringLiteral("—"));
    lines << QStringLiteral("Грубая геопривязка: ") + coordTextLocal(it.hasCoarseGeo, it.coarseLat, it.coarseLon);
    lines << QStringLiteral("Результат полного режима: ") + boolMarkLocal(it.hasResult);
    lines << QStringLiteral("Точки машин: ") + QString::number(it.vehiclePoints.size());
    if (!it.dateTimeOriginal.trimmed().isEmpty())
        lines << QStringLiteral("DateTimeOriginal: ") + it.dateTimeOriginal;
    else if (!it.dateTime.trimmed().isEmpty())
        lines << QStringLiteral("DateTime: ") + it.dateTime;
    if (!it.make.trimmed().isEmpty() || !it.model.trimmed().isEmpty())
        lines << QStringLiteral("Камера: ") + (it.make + QStringLiteral(" ") + it.model).trimmed();
    if (!it.geoRefPath.trimmed().isEmpty())
        lines << QStringLiteral("GeoRef: ") + it.geoRefPath;

    m_infoPanel->setText(lines.join(QStringLiteral("\n")));
    refreshItemsTable();
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
        it->locationSource = QStringLiteral("manual");
        if (it->uncertaintyM <= 0.0) it->uncertaintyM = 15.0;

        QString statusMessage;
        QString saveErr;
        saveGeoRef(*it, &saveErr);
        if (!saveErr.isEmpty()) {
            statusMessage = saveErr;
        }

        QString reprojErr;
        if (it->hasResult && reprojectVehiclesForCurrentCamera(*it, &reprojErr) && statusMessage.isEmpty()) {
            statusMessage = QStringLiteral("Ручная точка камеры задана. Положение машин пересчитано.");
        } else if (statusMessage.isEmpty() && !reprojErr.isEmpty()) {
            statusMessage = reprojErr;
        }

        m_editMode = EditMode::Idle;
        updateInfoPanel(*it);
        updateGeoControlsFromSelection();
        pushModelToQml();
        syncSelectedToQml();
        updateGeoStatus(statusMessage);
        return;
    }
    if (m_editMode == EditMode::SetCameraDirection) {
        if (!it->hasCameraPoint) {
            updateGeoStatus("Сначала задайте точку камеры.");
            m_editMode = EditMode::Idle;
            updateGeoControlsFromSelection();
            return;
        }

        it->hasCameraAzimuth = true;
        it->cameraAzimuthDeg = azimuthDegrees(it->cameraLat, it->cameraLon, lat, lon);

        QString statusMessage;
        QString saveErr;
        saveGeoRef(*it, &saveErr);
        if (!saveErr.isEmpty()) {
            statusMessage = saveErr;
        }

        QString reprojErr;
        if (it->hasResult && reprojectVehiclesForCurrentCamera(*it, &reprojErr) && statusMessage.isEmpty()) {
            statusMessage = QStringLiteral("Ручное направление камеры задано. Положение машин пересчитано.");
        } else if (statusMessage.isEmpty() && !reprojErr.isEmpty()) {
            statusMessage = reprojErr;
        }

        m_editMode = EditMode::Idle;
        updateInfoPanel(*it);
        updateGeoControlsFromSelection();
        pushModelToQml();
        syncSelectedToQml();
        updateGeoStatus(statusMessage);
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

    if (!it->hasCameraPoint) {
        updateGeoStatus("Сначала задайте точку камеры.");
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
    it->locationSource.clear();
    it->locationSource = defaultLocationSource(*it);

    if (it->hasResult) {
        applyVehicleGeo(it->result.artifacts, *it);
    } else {
        clearVehicleGeo(*it);
    }

    QString statusMessage;
    QString saveErr;
    saveGeoRef(*it, &saveErr);
    if (!saveErr.isEmpty())
        statusMessage = saveErr;

    m_editMode = EditMode::Idle;
    updateInfoPanel(*it);
    updateGeoControlsFromSelection();
    pushModelToQml();
    syncSelectedToQml();
    updateGeoStatus(statusMessage);
}
void MapTab::onImageSelected(const QString& imagePath)
{
    const QString p = imagePath.trimmed();
    if (p.isEmpty()) return;

    QFileInfo fi(p);
    if (!fi.exists() || !fi.isFile()) return;

    const QString ap = fi.absoluteFilePath();
    const QString key = normalizedImageKey(ap);

    auto existing = m_items.find(key);
    if (existing != m_items.end()) {
        selectItem(key);
        return;
    }

    Item it;
    it.imagePath = ap;
    it.geoRefPath = geoRefSidecarPath(it.imagePath);

    readExifMini(it.imagePath, it);

    if (!it.hasGps) {
        QJsonObject exifObj;
        QString err;
        if (readExifViaRunnerPreviewMap(m_cfg, it.imagePath, exifObj, err)) {
            applyRunnerExif(exifObj, it);
        }
    }

    QString geoErr;
    loadGeoRef(it.imagePath, it, &geoErr);
    if (it.locationSource.isEmpty())
        it.locationSource = defaultLocationSource(it);

    m_items.insert(key, it);
    pushModelToQml();
    selectItem(key);
}
void MapTab::onResultReady(const QString& imagePath, const ModuleResult& r)
{
    const QString p = imagePath.trimmed();
    if (p.isEmpty()) return;

    const QString ap = QFileInfo(p).absoluteFilePath();
    const QString key = normalizedImageKey(ap);

    auto it = m_items.find(key);
    if (it == m_items.end()) {
        Item ni;
        ni.imagePath = ap;
        ni.geoRefPath = geoRefSidecarPath(ap);
        readExifMini(ni.imagePath, ni);

        QString geoErr;
        loadGeoRef(ni.imagePath, ni, &geoErr);
        ni.locationSource = defaultLocationSource(ni);

        it = m_items.insert(key, ni);
    }

    it->hasResult = true;
    it->result = r;

    if (!r.exif.isEmpty()) {
        applyRunnerExif(r.exif, *it);
        if (!it->hasCameraPoint && it->locationSource.isEmpty())
            it->locationSource = defaultLocationSource(*it);
    }

    applyCoarseGeoSearch(r.artifacts, *it);
    applyCameraRefine(r.artifacts, *it);
    applyVehicleGeo(r.artifacts, *it);
    if (it->hasCameraPoint)
        reprojectVehiclesForCurrentCamera(*it, nullptr);

    pushModelToQml();

    if (m_selected.isEmpty() || m_selected == key || !m_items.contains(m_selected)) {
        selectItem(key);
        return;
    }

    syncSelectedToQml();
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
