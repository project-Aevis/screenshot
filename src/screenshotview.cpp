/*
 * Copyright (C) 2021 CutefishOS Team.
 * Copyright (C) 2026 Project Aevis Contributors.
 *
 * Author:     Reion Wong <reionwong@gmail.com>
 * Author:     Project Aevis Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "screenshotview.h"

#include <QClipboard>
#include <QEventLoop>
#include <QTimer>
#include <QThreadPool>
#include <QMutex>
#include <QMutexLocker>

#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QScreen>
#include <QPixmap>
#include <QStandardPaths>
#include <QDateTime>

#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusConnection>
#include <QDBusPendingCall>

#include <QFile>
#include <QDir>
#include <QUrl>
#include <QProcess>
#include <QMimeData>
#include <QBuffer>
#include <QDebug>

#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

class ScreenshotImageProvider : public QQuickImageProvider
{
public:
    ScreenshotImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
    {
        Q_UNUSED(id);
        QMutexLocker locker(&m_mutex);
        if (m_image.isNull()) {
            if (size) *size = QSize(1, 1);
            QImage dummy(1, 1, QImage::Format_ARGB32_Premultiplied);
            dummy.fill(Qt::transparent);
            return dummy;
        }
        if (size) {
            *size = m_image.size();
        }
        if (!requestedSize.isEmpty() && requestedSize != m_image.size()) {
            return m_image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return m_image;
    }

    void setImage(const QImage &image)
    {
        QMutexLocker locker(&m_mutex);
        m_image = image;
    }

    QImage image() const
    {
        QMutexLocker locker(&m_mutex);
        return m_image;
    }

private:
    mutable QMutex m_mutex;
    QImage m_image;
};

static bool readExact(int fd, uchar *data, qsizetype size)
{
    qsizetype offset = 0;
    while (offset < size) {
        const ssize_t n = read(fd, data + offset, size - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        offset += n;
    }
    return true;
}

static QImage readKWinRawImage(const QVariantMap &results, int readFd)
{
    const QString type = results.value(QStringLiteral("type")).toString();
    if (type != QStringLiteral("raw")) {
        return QImage();
    }

    const int width = results.value(QStringLiteral("width")).toUInt();
    const int height = results.value(QStringLiteral("height")).toUInt();
    const int stride = results.value(QStringLiteral("stride")).toUInt();
    const auto format = static_cast<QImage::Format>(results.value(QStringLiteral("format")).toUInt());

    if (width <= 0 || height <= 0 || stride <= 0) {
        return QImage();
    }

    QImage image(width, height, format);
    if (image.isNull()) {
        return QImage();
    }

    const qsizetype sourceStride = stride;
    const qsizetype targetStride = image.bytesPerLine();
    if (sourceStride == targetStride) {
        if (!readExact(readFd, image.bits(), targetStride * height)) {
            return QImage();
        }
    } else {
        QByteArray row;
        row.resize(sourceStride);
        const qsizetype copyBytes = qMin(sourceStride, targetStride);
        for (int y = 0; y < height; ++y) {
            if (!readExact(readFd, reinterpret_cast<uchar *>(row.data()), sourceStride)) {
                return QImage();
            }
            uchar *targetRow = image.scanLine(y);
            std::memcpy(targetRow, row.constData(), size_t(copyBytes));
            if (copyBytes < targetStride) {
                std::memset(targetRow + copyBytes, 0, size_t(targetStride - copyBytes));
            }
        }
    }

    return image;
}

static void setClipboardImageData(const QImage &image)
{
    QClipboard *clipboard = qGuiApp->clipboard();

    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    const QString tmpPath = QStringLiteral("/tmp/cutefish-latest-screenshot.png");
    const QString cachePath = QStringLiteral("%1/latest-screenshot.png").arg(cacheDir);

    QByteArray pngData;
    QBuffer buffer(&pngData);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");

    QFile tmpFile(tmpPath);
    if (tmpFile.open(QIODevice::WriteOnly))
        tmpFile.write(pngData);

    QFile cacheFile(cachePath);
    if (cacheFile.open(QIODevice::WriteOnly))
        cacheFile.write(pngData);

    if (clipboard) {
        const QList<QUrl> urls = {QUrl::fromLocalFile(tmpPath), QUrl::fromLocalFile(cachePath)};

        QMimeData *mimeData = new QMimeData();
        mimeData->setImageData(image);
        mimeData->setData(QStringLiteral("image/png"), pngData);
        mimeData->setUrls(urls);
        mimeData->setText(tmpPath);
        clipboard->setMimeData(mimeData, QClipboard::Clipboard);

        if (clipboard->supportsSelection()) {
            QMimeData *selMime = new QMimeData();
            selMime->setImageData(image);
            selMime->setData(QStringLiteral("image/png"), pngData);
            selMime->setUrls(urls);
            selMime->setText(tmpPath);
            clipboard->setMimeData(selMime, QClipboard::Selection);
        }
    }

    // Persist clipboard via xclip & wl-copy shell command execution
    QString cmd = QStringLiteral("xclip -selection clipboard -t image/png -i '%1' 2>/dev/null || wl-copy --type image/png < '%1' 2>/dev/null").arg(tmpPath);
    QProcess::startDetached(QStringLiteral("sh"), QStringList() << QStringLiteral("-c") << cmd);
}

ScreenshotView::ScreenshotView(QQuickView *parent)
    : QQuickView(parent)
    , m_imageProvider(new ScreenshotImageProvider())
{
    engine()->addImageProvider(QStringLiteral("screenshot"), m_imageProvider);
    rootContext()->setContextProperty(QStringLiteral("view"), this);

    setFlags(Qt::FramelessWindowHint);
    setColor(Qt::transparent);
    setScreen(qGuiApp->primaryScreen());
    setResizeMode(QQuickView::SizeRootObjectToView);
    setIcon(QIcon::fromTheme(QStringLiteral("aevis-screenshot")));
    setTitle(QStringLiteral("Screenshot"));
    if (screen()) {
        setGeometry(screen()->geometry());
    }
}

ScreenshotView::~ScreenshotView()
{
}

QImage ScreenshotView::captureKWin()
{
    QDBusInterface iface(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/org/kde/KWin/ScreenShot2"),
        QStringLiteral("org.kde.KWin.ScreenShot2"),
        QDBusConnection::sessionBus());

    if (!iface.isValid()) {
        return QImage();
    }

    QVariantMap options;
    options.insert(QStringLiteral("native-resolution"), true);

    QString screenName = screen() ? screen()->name() : QString();

    // 1. Try CaptureScreen with active screen name
    if (!screenName.isEmpty()) {
        int pipeFds[2];
        if (pipe2(pipeFds, O_CLOEXEC) == 0) {
            QDBusReply<QVariantMap> reply = iface.call(
                QStringLiteral("CaptureScreen"),
                screenName,
                options,
                QVariant::fromValue(QDBusUnixFileDescriptor(pipeFds[1])));
            ::close(pipeFds[1]);
            if (reply.isValid()) {
                QImage img = readKWinRawImage(reply.value(), pipeFds[0]);
                ::close(pipeFds[0]);
                if (!img.isNull()) {
                    return img;
                }
            } else {
                ::close(pipeFds[0]);
            }
        }
    }

    // 2. Try CaptureActiveScreen
    {
        int pipeFds[2];
        if (pipe2(pipeFds, O_CLOEXEC) == 0) {
            QDBusReply<QVariantMap> reply = iface.call(
                QStringLiteral("CaptureActiveScreen"),
                options,
                QVariant::fromValue(QDBusUnixFileDescriptor(pipeFds[1])));
            ::close(pipeFds[1]);
            if (reply.isValid()) {
                QImage img = readKWinRawImage(reply.value(), pipeFds[0]);
                ::close(pipeFds[0]);
                if (!img.isNull()) {
                    return img;
                }
            } else {
                ::close(pipeFds[0]);
            }
        }
    }

    // 3. Try CaptureWorkspace
    {
        int pipeFds[2];
        if (pipe2(pipeFds, O_CLOEXEC) == 0) {
            QDBusReply<QVariantMap> reply = iface.call(
                QStringLiteral("CaptureWorkspace"),
                options,
                QVariant::fromValue(QDBusUnixFileDescriptor(pipeFds[1])));
            ::close(pipeFds[1]);
            if (reply.isValid()) {
                QImage img = readKWinRawImage(reply.value(), pipeFds[0]);
                ::close(pipeFds[0]);
                if (!img.isNull()) {
                    return img;
                }
            } else {
                ::close(pipeFds[0]);
            }
        }
    }

    return QImage();
}

QImage ScreenshotView::captureExternal()
{
    const QString tmpPath = QStringLiteral("/tmp/cutefish-screenshot.png");
    QFile::remove(tmpPath);

    // 1. Try spectacle in background mode
    {
        QProcess pr;
        pr.start(QStringLiteral("spectacle"), QStringList() << QStringLiteral("-b") << QStringLiteral("-n") << QStringLiteral("-o") << tmpPath);
        if (pr.waitForFinished(6000) && QFile::exists(tmpPath)) {
            QImage img(tmpPath);
            if (!img.isNull()) {
                return img;
            }
        }
    }

    // 2. Try grim (standard Wayland / wlroots screenshot tool)
    {
        if (QProcess::execute(QStringLiteral("grim"), QStringList() << tmpPath) == 0 && QFile::exists(tmpPath)) {
            QImage img(tmpPath);
            if (!img.isNull()) {
                return img;
            }
        }
    }

    // 3. Try Qt QScreen::grabWindow (standard X11 / XWayland root window capture)
    if (screen()) {
        QPixmap p = screen()->grabWindow(0);
        if (!p.isNull()) {
            QImage img = p.toImage();
            if (!img.isNull()) {
                return img;
            }
        }
    }

    // 4. Try import (ImageMagick X11)
    {
        if (QProcess::execute(QStringLiteral("import"), QStringList() << QStringLiteral("-window") << QStringLiteral("root") << tmpPath) == 0 && QFile::exists(tmpPath)) {
            QImage img(tmpPath);
            if (!img.isNull()) {
                return img;
            }
        }
    }

    return QImage();
}

QImage ScreenshotView::captureScreen()
{
    // 1. Try native KWin ScreenShot2 D-Bus interface (fastest, zero-copy, <20ms)
    QImage img = captureKWin();
    if (!img.isNull()) {
        return img;
    }

    // 2. Fallbacks (Spectacle, grim, QScreen::grabWindow, ImageMagick)
    return captureExternal();
}

void ScreenshotView::start()
{
    m_capturedImage = captureScreen();

    if (m_capturedImage.isNull()) {
        qWarning() << "Screenshot: Unable to capture screen from any backend!";
        qGuiApp->quit();
        return;
    }

    m_imageProvider->setImage(m_capturedImage);

    // Save temporary file in background for backwards compatibility / external tools
    QImage copy = m_capturedImage;
    QThreadPool::globalInstance()->start([copy]() {
        copy.save(QStringLiteral("/tmp/aevis-screenshot.png"), "PNG");
        copy.save(QStringLiteral("/tmp/cutefish-screenshot.png"), "PNG");
    });

    if (source().isEmpty()) {
        setSource(QUrl(QStringLiteral("qrc:/qml/main.qml")));
    }

    if (screen()) {
        setGeometry(screen()->geometry());
    }

    setVisible(true);
    setKeyboardGrabEnabled(true);

    emit refresh();
}

void ScreenshotView::delay(int value)
{
    QEventLoop waitLoop;
    QTimer::singleShot(value, &waitLoop, &QEventLoop::quit);
    waitLoop.exec();

    start();
}

void ScreenshotView::captureFullscreenDirect()
{
    m_capturedImage = captureScreen();

    if (m_capturedImage.isNull()) {
        qWarning() << "Cutefish Screenshot: Unable to capture fullscreen from any backend!";
        qGuiApp->quit();
        return;
    }

    copyToClipboard(QRect());
}

void ScreenshotView::quit()
{
    qGuiApp->quit();
}

void ScreenshotView::saveFile(QRect rect)
{
    setVisible(false);

    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QString fileName = QString("%1/Screenshot_%2.png")
                              .arg(desktopPath)
                              .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));

    QImage image = m_capturedImage;
    if (image.isNull()) {
        image.load(QStringLiteral("/tmp/cutefish-screenshot.png"));
    }
    if (image.isNull() && screen()) {
        QPixmap p = screen()->grabWindow(0);
        image = p.toImage();
    }

    qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    QRect cropRect(qRound(rect.x() * dpr), qRound(rect.y() * dpr), qRound(rect.width() * dpr), qRound(rect.height() * dpr));
    if (cropRect.isEmpty() || cropRect.width() <= 0 || cropRect.height() <= 0) {
        cropRect = image.rect();
    } else {
        cropRect = cropRect.intersected(image.rect());
    }

    QImage cropped = image.copy(cropRect);
    bool saved = cropped.save(fileName);

    // Always keep last taken screenshot in clipboard
    setClipboardImageData(cropped);

    if (saved) {
        QDBusInterface iface(QStringLiteral("org.freedesktop.Notifications"),
                             QStringLiteral("/org/freedesktop/Notifications"),
                             QStringLiteral("org.freedesktop.Notifications"),
                             QDBusConnection::sessionBus());
        if (iface.isValid()) {
            QString iconName = QIcon::hasThemeIcon(QStringLiteral("aevis-screenshot"))
                                   ? QStringLiteral("aevis-screenshot")
                                   : QStringLiteral("cutefish-screenshot");
            QList<QVariant> args;
            args << QStringLiteral("aevis-screenshot");
            args << ((unsigned int) 0);
            args << iconName;
            args << QString();
            args << tr("The picture has been saved to %1 and copied to clipboard").arg(fileName);
            args << QStringList();
            args << QVariantMap();
            args << (int) 10;
            iface.asyncCallWithArgumentList(QStringLiteral("Notify"), args);
        }
    }

    removeTmpFile();
    QTimer::singleShot(400, qGuiApp, &QGuiApplication::quit);
}

void ScreenshotView::copyToClipboard(QRect rect)
{
    setVisible(false);

    QImage image = m_capturedImage;
    if (image.isNull()) {
        if (!image.load(QStringLiteral("/tmp/aevis-screenshot.png"))) {
            image.load(QStringLiteral("/tmp/cutefish-screenshot.png"));
        }
    }
    if (image.isNull() && screen()) {
        QPixmap p = screen()->grabWindow(0);
        image = p.toImage();
    }

    qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    QRect cropRect(qRound(rect.x() * dpr), qRound(rect.y() * dpr), qRound(rect.width() * dpr), qRound(rect.height() * dpr));
    if (cropRect.isEmpty() || cropRect.width() <= 0 || cropRect.height() <= 0) {
        cropRect = image.rect();
    } else {
        cropRect = cropRect.intersected(image.rect());
    }

    QImage cropped = image.copy(cropRect);
    setClipboardImageData(cropped);

    QDBusInterface iface(QStringLiteral("org.freedesktop.Notifications"),
                         QStringLiteral("/org/freedesktop/Notifications"),
                         QStringLiteral("org.freedesktop.Notifications"),
                         QDBusConnection::sessionBus());
    if (iface.isValid()) {
        QString iconName = QIcon::hasThemeIcon(QStringLiteral("aevis-screenshot"))
                               ? QStringLiteral("aevis-screenshot")
                               : QStringLiteral("cutefish-screenshot");
        QList<QVariant> args;
        args << QStringLiteral("aevis-screenshot");
        args << ((unsigned int) 0);
        args << iconName;
        args << QString();
        args << tr("The picture has been saved to the clipboard");
        args << QStringList();
        args << QVariantMap();
        args << (int) 10;
        iface.asyncCallWithArgumentList(QStringLiteral("Notify"), args);
    }

    removeTmpFile();

    QTimer::singleShot(200, qGuiApp, &QGuiApplication::quit);
}

void ScreenshotView::removeTmpFile()
{
    QFile(QStringLiteral("/tmp/aevis-screenshot.png")).remove();
    QFile(QStringLiteral("/tmp/cutefish-screenshot.png")).remove();
}
