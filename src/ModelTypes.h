#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>

// Структура для одного детектора (машины)
struct Detection {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    double conf = 0.0;
    int clsId = -1;
    QString clsName;
    QJsonObject meta;
};

// Основной результат работы любого модуля + расширения схемы v1
struct ModuleResult {
    QString moduleId;
    int imageW = 0;
    int imageH = 0;
    QString deviceUsed;
    QStringList warnings;

    QString annotatedImagePath;
    QString cleanedImagePath;

    QJsonObject artifacts;
    QJsonObject timingsMs;

    QVector<Detection> detections;

    // VK Result v1 (если пришёл envelope)
    QJsonObject meta;          // root.meta
    QStringList consoleStdout; // root.console.stdout
    QStringList consoleStderr; // root.console.stderr
    QJsonArray images;         // root.images
    QJsonArray tables;         // root.tables
    QJsonArray plots;          // root.plots
    QJsonObject exif;          // root.exif

    static bool fromJson(const QJsonObject& root, ModuleResult& out, QString& err) {
        auto clearV1 = [&]() {
            out.meta = QJsonObject();
            out.consoleStdout.clear();
            out.consoleStderr.clear();
            out.images = QJsonArray();
            out.tables = QJsonArray();
            out.plots = QJsonArray();
            out.exif = QJsonObject();
        };

        auto parseModule = [&](const QJsonObject& o) -> bool {
            auto getStr = [&](const char* k)->QString { return o.value(k).toString(); };

            out.moduleId = getStr("module_id");
            out.imageW = o.value("image_w").toInt();
            out.imageH = o.value("image_h").toInt();
            out.deviceUsed = getStr("device_used");

            out.annotatedImagePath = getStr("annotated_image_path");
            out.cleanedImagePath   = getStr("cleaned_image_path");

            out.artifacts = o.value("artifacts").toObject();
            out.timingsMs = o.value("timings_ms").toObject();

            out.warnings.clear();
            for (const auto& v : o.value("warnings").toArray()) out.warnings.push_back(v.toString());

            out.detections.clear();
            QJsonArray arr = o.value("detections").toArray();
            out.detections.reserve(arr.size());
            for (const auto& dv : arr) {
                QJsonObject d = dv.toObject();
                Detection det;
                QJsonArray bb = d.value("bbox_xyxy").toArray();
                if (bb.size() != 4) { err = "Invalid bbox_xyxy size"; return false; }
                det.x1 = bb.at(0).toInt();
                det.y1 = bb.at(1).toInt();
                det.x2 = bb.at(2).toInt();
                det.y2 = bb.at(3).toInt();
                det.conf = d.value("conf").toDouble();
                det.clsId = d.value("cls_id").toInt(-1);
                det.clsName = d.value("cls_name").toString();
                det.meta = d.value("meta").toObject();
                out.detections.push_back(det);
            }

            if (out.moduleId.isEmpty()) { err = "module_id is empty"; return false; }
            return true;
        };

        clearV1();

        const QString schema = root.value("vk_schema").toString();
        if (!schema.isEmpty()) {
            QJsonObject mod = root.value("module").toObject();
            if (mod.isEmpty()) { err = "vk_schema set but module object is empty"; return false; }

            out.meta = root.value("meta").toObject();
            out.images = root.value("images").toArray();
            out.tables = root.value("tables").toArray();
            out.plots  = root.value("plots").toArray();
            out.exif   = root.value("exif").toObject();

            QJsonObject c = root.value("console").toObject();
            for (const auto& v : c.value("stdout").toArray()) out.consoleStdout.push_back(v.toString());
            for (const auto& v : c.value("stderr").toArray()) out.consoleStderr.push_back(v.toString());

            return parseModule(mod);
        }

        return parseModule(root);
    }
};