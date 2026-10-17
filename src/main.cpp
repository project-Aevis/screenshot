/*
 * Copyright (C) 2021 CutefishOS Team.
 * Copyright (C) 2026 Project Aevis Contributors.
 *
 * Author:     Reion Wong <reionwong@gmail.com>
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

#include <QApplication>
#include <QDBusConnection>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QTranslator>
#include <QLocale>
#include <QFile>
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "screenshotview.h"

int main(int argc, char *argv[])
{
#ifdef __GLIBC__
    mallopt(M_ARENA_MAX, 1);
    mallopt(M_TRIM_THRESHOLD, 64 * 1024);
    mallopt(M_MMAP_THRESHOLD, 32 * 1024);
#endif

    // Dynamic EGL vendor selection to prevent NVIDIA driver bloat on Intel/AMD GPUs
    if (!QFile::exists(QStringLiteral("/dev/nvidia0")) &&
        QFile::exists(QStringLiteral("/usr/share/glvnd/egl_vendor.d/50_mesa.json"))) {
        qputenv("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/50_mesa.json");
    }

    qputenv("QSG_RENDER_LOOP", "basic");

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("aevis-screenshot"));
    app.setDesktopFileName(QStringLiteral("aevis-screenshot"));
    app.setOrganizationName(QStringLiteral("aevis"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("aevis-screenshot")));

    QCommandLineOption delayOption(QStringList() << "d" << "delay", "Delay Screenshot", "NUM");
    QCommandLineOption areaOption(QStringList() << "a" << "area", "Capture Area");
    QCommandLineOption fullOption(QStringList() << "f" << "fullscreen", "Capture Fullscreen");

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Project Aevis Screenshot"));
    parser.addHelpOption();
    parser.addOption(delayOption);
    parser.addOption(areaOption);
    parser.addOption(fullOption);
    parser.process(app);

    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.registerService(QStringLiteral("org.aevis.Screenshot"));
    bus.registerService(QStringLiteral("com.cutefish.Screenshot"));

    const QStringList searchPaths = {
        QStringLiteral("/usr/share/aevis-screenshot/translations/"),
        QStringLiteral("/usr/share/cutefish-screenshot/translations/"),
        QStringLiteral("/usr/local/share/cutefish-screenshot/translations/"),
        QDir::homePath() + QStringLiteral("/.local/share/cutefish-screenshot/translations/")
    };

    for (const QString &path : searchPaths) {
        QString qmFilePath = QString("%1%2.qm").arg(path, QLocale::system().name());
        if (QFile::exists(qmFilePath)) {
            QTranslator *translator = new QTranslator(QApplication::instance());
            if (translator->load(qmFilePath)) {
                QApplication::installTranslator(translator);
                break;
            } else {
                translator->deleteLater();
            }
        }
    }

    ScreenshotView view;
    if (parser.isSet(fullOption)) {
        if (parser.isSet(delayOption)) {
            QEventLoop waitLoop;
            QTimer::singleShot(parser.value(delayOption).toInt(), &waitLoop, &QEventLoop::quit);
            waitLoop.exec();
        }
        view.captureFullscreenDirect();
    } else if (parser.isSet(delayOption)) {
        view.delay(parser.value(delayOption).toInt());
    } else {
        view.start();
    }

    return app.exec();
}
