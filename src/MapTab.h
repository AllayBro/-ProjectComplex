#pragma once
#include <QWidget>
#include <QHash>

#include "AppConfig.h"
#include "ModelTypes.h"

class QQuickWidget;
class QQuickItem;
class QLabel;
class QTextEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class MapTab : public QWidget {
    Q_OBJECT
public:
    explicit MapTab(const AppConfig& cfg, QWidget* parent = nullptr);

public slots:
    void onImageSelected(const QString& imagePath);
    void onResultReady(const QString& imagePath, const ModuleResult& r);

private slots:
    void onMarkerClicked(const QString& imagePath);

    void probeNetworkNow();
    void onProbeFinished();
    void onProbeTimeout();

private:
    struct Item {
        QString imagePath;

        bool hasGps = false;
        double lat = 0.0;
        double lon = 0.0;

        QString make;
        QString model;
        QString dateTime;
        QString dateTimeOriginal;

        QJsonObject exif;

        bool hasResult = false;
        ModuleResult result;
    };

    AppConfig m_cfg;

    QQuickWidget* m_quick = nullptr;

    QLabel* m_netStatus = nullptr;
    QLabel* m_preview = nullptr;
    QTextEdit* m_info = nullptr;

    QHash<QString, Item> m_items;
    QString m_selected;

    QNetworkAccessManager* m_nam = nullptr;
    QTimer* m_probeTimer = nullptr;
    QTimer* m_probeTimeout = nullptr;
    QNetworkReply* m_probeReply = nullptr;

    bool m_onlineOk = true;
    bool m_probeSeen = false;

    void initQml();
    void applyEffectiveModeToQml();
    void pushModelToQml();
    void selectItem(const QString& imagePath);
    void updateInfoPanel(const Item& it);
    void updateNetLabel();

    static bool readExifMini(const QString& imagePath, Item& out);
    static void applyRunnerExif(const QJsonObject& exifObj, Item& out);
};