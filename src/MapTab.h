#pragma once
#include <QWidget>
#include <QHash>
#include <QVariant>
#include "AppConfig.h"
#include "ModelTypes.h"

class QQuickWidget;
class QQuickItem;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QPushButton;

class MapTab : public QWidget {
    Q_OBJECT
public:
    explicit MapTab(const AppConfig& cfg, QWidget* parent = nullptr);

public slots:
    void onImageSelected(const QString& imagePath);
    void onResultReady(const QString& imagePath, const ModuleResult& r);

private slots:
    void onMarkerClicked(const QString& imagePath);
    void onMapClicked(double lat, double lon);

    void onSetCameraPointMode();
    void onSetDirectionMode();
    void onClearGeoRef();
    void onSaveGeoRef();

    void probeNetworkNow();
    void onProbeFinished();
    void onProbeTimeout();



private:
    enum class EditMode {
        Idle,
        SetCameraPoint,
        SetCameraDirection
    };
    struct Item {
        QString imagePath;

        bool hasGps = false;
        double lat = 0.0;
        double lon = 0.0;

        bool hasCameraPoint = false;
        double cameraLat = 0.0;
        double cameraLon = 0.0;

        bool hasCameraAzimuth = false;
        double cameraAzimuthDeg = 0.0;

        double uncertaintyM = 15.0;
        QString locationSource;
        QString geoRefPath;

        bool hasRefinedCamera = false;
        double refinedLat = 0.0;
        double refinedLon = 0.0;

        bool hasRefinedAzimuth = false;
        double refinedAzimuthDeg = 0.0;

        double refinedUncertaintyM = 0.0;
        double refinedConfidence = 0.0;
        bool refinedNeedsManualReview = false;

        QString refinedMethod;
        QString refinedSeedSource;
        QString cameraRefineJsonPath;

        bool hasVehicleGeo = false;
        double vehicleLat = 0.0;
        double vehicleLon = 0.0;
        double vehicleDistanceM = 0.0;
        double vehicleBearingDeg = 0.0;
        double vehicleConfidence = 0.0;
        bool vehicleNeedsManualReview = false;
        QString vehicleGeoStatus;
        QString vehicleGeoJsonPath;
        QVariantList vehiclePoints;
        QString make;
        QString model;
        QString dateTime;
        QString dateTimeOriginal;

        QJsonObject exif;

        bool hasResult = false;
        ModuleResult result;

        bool hasCoarseGeo = false;
        double coarseLat = 0.0;
        double coarseLon = 0.0;
        double coarseRadiusM = 0.0;
        double coarseConfidence = 0.0;
        QString coarseMethod;
        QString coarseSeedSource;
        QString coarseGeoJsonPath;
    };

    AppConfig m_cfg;

    QQuickWidget* m_quick = nullptr;

    QLabel* m_netStatus = nullptr;
    QLabel* m_geoStatus = nullptr;
    QPushButton* m_btnSetCameraPoint = nullptr;
    QPushButton* m_btnSetDirection = nullptr;
    QPushButton* m_btnClearGeoRef = nullptr;
    QPushButton* m_btnSaveGeoRef = nullptr;

    QHash<QString, Item> m_items;
    QString m_selected;

    QNetworkAccessManager* m_nam = nullptr;
    QTimer* m_probeTimer = nullptr;
    QTimer* m_probeTimeout = nullptr;
    QNetworkReply* m_probeReply = nullptr;

    bool m_onlineOk = true;
    bool m_probeSeen = false;
    EditMode m_editMode = EditMode::Idle;

    void initQml();
    void applyEffectiveModeToQml();
    void pushModelToQml();
    void selectItem(const QString& imagePath);
    void updateInfoPanel(const Item& it);
    void updateNetLabel();
    void updateGeoStatus(const QString& text = QString());
    void updateGeoControlsFromSelection();
    void syncSelectedToQml();
    void applyCoarseGeoSearch(const QJsonObject& artifactsObj, Item& out);

    static bool readExifMini(const QString& imagePath, Item& out);
    static void applyRunnerExif(const QJsonObject& exifObj, Item& out);
    static void applyCameraRefine(const QJsonObject& artifactsObj, Item& out);
    static void applyVehicleGeo(const QJsonObject& artifactsObj, Item& out);
    static bool itemDisplayCoords(const Item& it, double& lat, double& lon, QString* source = nullptr);
    static QString defaultLocationSource(const Item& it);
    static QString geoRefSidecarPath(const QString& imagePath);
    static bool loadGeoRef(const QString& imagePath, Item& out, QString* err = nullptr);
    static bool saveGeoRef(const Item& it, QString* err = nullptr);
    static double normalizeAzimuth(double value);
    static double azimuthDegrees(double lat1, double lon1, double lat2, double lon2);
    static QString autoSidecarLocationSource(const Item& it);
    static double autoSidecarUncertaintyM(const Item& it);
    static bool isManualReviewRecommended(const Item& it);
    static bool isLocalExifRefine(const Item& it);
};