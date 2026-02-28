#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSplashScreen>
#include <QTimer>

#include <algorithm>

#include "MainWindow.h"
#include "AppConfig.h"

static QImage loadFirstImage(const QStringList& candidates)
{
    for (const QString& p : candidates) {
        QImage img;
        if (p.startsWith(":/")) img = QImage(p);
        else img.load(p);
        if (!img.isNull()) return img;
    }
    return {};
}

static QImage cropTransparent(const QImage& src)
{
    if (src.isNull() || !src.hasAlphaChannel()) return src;

    const int w = src.width();
    const int h = src.height();

    int left = w, right = -1, top = h, bottom = -1;

    for (int y = 0; y < h; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            if (qAlpha(line[x]) > 0) {
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
        }
    }

    if (right < left || bottom < top) return src;
    return src.copy(QRect(left, top, right - left + 1, bottom - top + 1));
}

static QImage centerCropSquare(const QImage& src, int size)
{
    if (src.isNull()) return {};

    QImage scaled = src.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    const int x = std::max(0, (scaled.width() - size) / 2);
    const int y = std::max(0, (scaled.height() - size) / 2);

    return scaled.copy(QRect(x, y, size, size));
}

static QIcon buildWindowIcon(const QImage& logoCropped)
{
    QIcon icon;
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};

    for (int s : sizes) {
        QImage sq(s, s, QImage::Format_ARGB32_Premultiplied);
        sq.fill(Qt::transparent);

        if (!logoCropped.isNull()) {
            QImage cropped = centerCropSquare(logoCropped, s);
            QPainter p(&sq);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(0, 0, cropped);
        }
        icon.addPixmap(QPixmap::fromImage(sq));
    }
    return icon;
}

static QPixmap buildSplashBase(const QImage& logoCropped, int w, int h, int bottomPad)
{
    QImage base(w, h, QImage::Format_ARGB32_Premultiplied);
    // Важно: фон должен быть полностью прозрачным,
    // иначе QSplashScreen будет выглядеть как непрозрачный прямоугольник.
    base.fill(Qt::transparent);

    if (!logoCropped.isNull()) {
        const int drawH = h - bottomPad;
        const QImage scaled = logoCropped.scaled(w, drawH, Qt::KeepAspectRatio, Qt::SmoothTransformation);

        QPainter p(&base);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const int x = (w - scaled.width()) / 2;
        const int y = (drawH - scaled.height()) / 2;
        p.drawImage(x, y, scaled);
    }
    return QPixmap::fromImage(base);
}

static void renderSplash(QSplashScreen& splash, const QPixmap& base, int percent, const QString& text)
{
    const int p = std::clamp(percent, 0, 100);

    QPixmap px = base;
    QPainter painter(&px);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int padX = 18;
    const int padBottom = 16;

    const int barH = 10;
    const int barY = px.height() - padBottom - barH;
    QRect barRect(padX, barY, px.width() - 2 * padX, barH);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 140));
    painter.drawRoundedRect(barRect, 6, 6);

    const int fillW = (barRect.width() * p) / 100;
    if (fillW > 0) {
        QRect fillRect(barRect.x(), barRect.y(), fillW, barRect.height());
        painter.setBrush(QColor(0, 160, 255, 220));
        painter.drawRoundedRect(fillRect, 6, 6);
    }

    const int textBoxH = 30;
    QRect textRect(padX, barRect.y() - 10 - textBoxH, px.width() - 2 * padX, textBoxH);

    painter.setBrush(QColor(0, 0, 0, 140));
    painter.drawRoundedRect(textRect, 10, 10);

    painter.setPen(Qt::white);
    painter.drawText(textRect, Qt::AlignCenter, text);

    splash.setPixmap(px);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    app.setFont(QFont("Segoe UI", 9));
    QCoreApplication::setOrganizationName("AllayBro");
    QCoreApplication::setApplicationName("vk_qt_app");

    const QString appDir = QCoreApplication::applicationDirPath();
    QDir rootDir(appDir);
    // Если запущены из cmake-build-debug, подняться на уровень выше
    if (rootDir.dirName() == "cmake-build-debug" || rootDir.dirName() == "bin") {
        rootDir.cdUp();
    }
    const QString projectRoot = rootDir.absolutePath();

    QImage logoImg = loadFirstImage({
        ":/icons/app_icon_1024.png",
        ":/icons/app_icon_512.png",
        ":/icons/app_icon_256.png",
        ":/icons/app_icon.png",
        projectRoot + "/assets/icons/app_icon.png"
    });

    const QImage logoCropped = cropTransparent(logoImg);

    if (!logoCropped.isNull())
        app.setWindowIcon(buildWindowIcon(logoCropped));
    else
        app.setWindowIcon(QIcon(":/icons/app_icon_256.png"));

    const int splashW = 760;
    const int splashH = 520;
    const int bottomPad = 90;

    const QPixmap splashBase = buildSplashBase(logoCropped, splashW, splashH, bottomPad);

    QSplashScreen splash(splashBase);
    splash.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::SplashScreen);
    splash.setAttribute(Qt::WA_TranslucentBackground, true);
    splash.setStyleSheet("background: transparent;");

    QElapsedTimer splashTimer;
    splashTimer.start();

    splash.show();
    renderSplash(splash, splashBase, 5, "Инициализация...");
    app.processEvents();

    renderSplash(splash, splashBase, 15, "Чтение конфигурации...");
    app.processEvents();

    AppConfig cfg = AppConfig::loadOrDie(projectRoot);

    auto setProgress = [&](int percent, const QString& msg) {
        renderSplash(splash, splashBase, percent, msg);
        app.processEvents();
    };

    setProgress(35, "Подготовка параметров...");

    MainWindow w(cfg, projectRoot, [&](int percent, const QString& msg) {
        setProgress(percent, msg);
    });

    setProgress(98, "Запуск...");
    w.show();

    const qint64 minSplashMs = 3000;
    const qint64 remainingMs = minSplashMs - splashTimer.elapsed();
    if (remainingMs > 0) {
        QEventLoop loop;
        QTimer::singleShot(int(remainingMs), &loop, &QEventLoop::quit);
        loop.exec();
    }

    setProgress(100, "Готово");
    splash.finish(&w);

    return app.exec();
}
