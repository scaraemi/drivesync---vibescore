/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <csignal>

#include <QApplication>
#include <QStyleHints>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QSettings>
#ifndef Q_OS_WASM
#include <QSslSocket>
#endif

#include "appfactory.h"
#include "internal/commandlineparser.h"
#include "global/iapplication.h"

#include "muse_framework_config.h"
#include "app_config.h"

#include "log.h"

// On Android and iOS the system crash reporters (tombstones, crash logs) are more useful than this handler
#if !defined(MUSE_MODULE_DIAGNOSTICS_CRASHPAD_CLIENT) && !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
#define MUE_INSTALL_CRASH_SIGNAL_HANDLER
#endif

#ifdef MUE_INSTALL_CRASH_SIGNAL_HANDLER
static void crashCallback(int signum)
{
    const char* signame = "UNKNOWN SIGNAME";
    const char* sigdescript = "";
    switch (signum) {
    case SIGILL:
        signame = "SIGILL";
        sigdescript = "Illegal Instruction";
        break;
    case SIGSEGV:
        signame = "SIGSEGV";
        sigdescript =  "Invalid memory reference";
        break;
    }
    LOGE() << "Oops! Application crashed with signal: [" << signum << "] " << signame << "-" << sigdescript;
    exit(EXIT_FAILURE);
}

#endif

static void app_init_qrc()
{
    Q_INIT_RESOURCE(app);

#ifdef Q_OS_WIN
    Q_INIT_RESOURCE(app_win);
#endif
}

int main(int argc, char** argv)
{
#ifdef MUE_INSTALL_CRASH_SIGNAL_HANDLER
    signal(SIGSEGV, crashCallback);
    signal(SIGILL, crashCallback);
    signal(SIGFPE, crashCallback);
#endif

    // ====================================================
    // Setup global Qt application variables
    // ====================================================

    app_init_qrc();

    qputenv("QT_STYLE_OVERRIDE", "Fusion");
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS) || defined(Q_OS_WASM)
    // The QML that was compiled when the app was built (its byte code, see muse_create_qml_module), but no cache files
    // written when the app runs. QML_DISABLE_DISK_CACHE turns off both: then all the QML is compiled at every start,
    // which took seconds on phones, tablets and in the browser
    qputenv("QML_DISK_CACHE", "aot");
#else
    qputenv("QML_DISABLE_DISK_CACHE", "true");
#endif

    // HACK: Workaround for crash #28840. This disables the incremental GC
    if (!qEnvironmentVariableIsSet("MU_QV4_GC_TIMELIMIT")) {
        qputenv("QV4_GC_TIMELIMIT", "0");
    }

    if (!qEnvironmentVariableIsSet("QT_QUICK_FLICKABLE_WHEEL_DECELERATION")) {
        qputenv("QT_QUICK_FLICKABLE_WHEEL_DECELERATION", "5000");
    }

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (qEnvironmentVariable("MU_QT_QPA_PLATFORM") != "offscreen") {
        qputenv("QT_QPA_PLATFORMTHEME", "gtk3");
    }

    //! NOTE Forced X11, with Wayland there are a number of problems now
    if (qEnvironmentVariable("MU_QT_QPA_PLATFORM") == "") {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
#endif

#ifdef Q_OS_WIN
    // NOTE: There are some problems with rendering the application window on some integrated graphics processors
    //       see https://github.com/musescore/MuseScore/issues/8270
    if (!qEnvironmentVariableIsSet("QT_OPENGL_BUGLIST")) {
        qputenv("QT_OPENGL_BUGLIST", ":/resources/win_opengl_buglist.json");
    }
#endif

    QGuiApplication::styleHints()->setMousePressAndHoldInterval(250);

#ifdef MUE_CONFIGURATION_IS_APPMOBILE
    // VibeScore keeps its settings and data separate from MuseScore Studio
    QCoreApplication::setApplicationName(VS_BRAND_NAME_MACHINE);
    QCoreApplication::setOrganizationName(VS_BRAND_ORGANIZATION);
    QCoreApplication::setOrganizationDomain(VS_BRAND_DOMAIN);
#else
// Can't use MUSE_APP_TITLE until next major release, because this "application name" is used to determine
// where user settings are stored. Changing it would result in all user settings being lost.
#ifdef MUSE_APP_UNSTABLE
    QCoreApplication::setApplicationName("MuseScore4Development");
#else
    QCoreApplication::setApplicationName("MuseScore4");
#endif
    QCoreApplication::setOrganizationName("MuseScore");
    QCoreApplication::setOrganizationDomain("musescore.org");
#endif
    QCoreApplication::setApplicationVersion(MUSE_APP_VERSION);

#ifdef MUE_CONFIGURATION_IS_APPMOBILE
    // Preferences > Appearance > Interface scale (IUiConfiguration::uiScale): Qt applies it to the screens when it starts
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        QSettings::setDefaultFormat(QSettings::IniFormat); // as muse::Settings
        const double uiScale = QSettings().value("ui/application/scale", 1.0).toDouble();
        if (uiScale >= 0.5 && uiScale <= 2.0 && !qFuzzyCompare(uiScale, 1.0)) {
            qputenv("QT_SCALE_FACTOR", QByteArray::number(uiScale));
        }
    }
#endif

#if !defined(Q_OS_WIN) && !defined(Q_OS_DARWIN) && !defined(Q_OS_WASM) && !defined(Q_OS_ANDROID)
    // Any OS that uses Freedesktop.org Desktop Entry Specification (e.g. Linux, BSD)
#ifndef MUSE_APP_INSTALL_SUFFIX
#define MUSE_APP_INSTALL_SUFFIX ""
#endif
    QGuiApplication::setDesktopFileName("org.musescore.MuseScore" MUSE_APP_INSTALL_SUFFIX);
#endif

#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    // Every window with the same 8-bit RGBA format. The QML windows ask for an alpha buffer (setDefaultAlphaBuffer
    // in GuiApp); windows created later without one (the widget dialogs such as Customize kit, the drag image)
    // could get an EGL config that does not match their Android surface on some devices: the app then aborted in
    // eglCreateWindowSurface ("Could not create the egl surface").
    {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setRedBufferSize(8);
        format.setGreenBufferSize(8);
        format.setBlueBufferSize(8);
        format.setAlphaBufferSize(8);
        QSurfaceFormat::setDefaultFormat(format);
    }
#endif

    using namespace muse;
    using namespace mu::app;

    auto fixSslBackend = []() {
#ifdef Q_OS_WIN
        // NOTE: Force schannel backend. Qt prefers OpenSSL when qopensslbackend.dll
        //       is present, which can crash on ABI mismatch with bundled OpenSSL.
        //       see https://github.com/musescore/MuseScore/issues/33401
        QSslSocket::setActiveBackend("schannel");
#endif
    };

    // ====================================================
    // Parse command line options
    // ====================================================
#ifdef MUE_ENABLE_CONSOLEAPP
    CommandLineParser commandLineParser;
    commandLineParser.init();
    commandLineParser.parse(argc, argv);

    IApplication::RunMode runMode = commandLineParser.runMode();
    QCoreApplication* qapp = nullptr;

    if (runMode == IApplication::RunMode::AudioPluginRegistration) {
        qapp = new QCoreApplication(argc, argv);
    } else {
        qapp = new QApplication(argc, argv);
    }

    fixSslBackend();

    commandLineParser.processBuiltinArgs(*qapp);
    CmdOptions opt = commandLineParser.options();

#else
    QCoreApplication* qapp = new QApplication(argc, argv);

    fixSslBackend();

    CmdOptions opt;
    opt.runMode = IApplication::RunMode::GuiApp;
#endif

    AppFactory f;
    std::shared_ptr<muse::IApplication> app = f.newApp(opt);

    app->setup();

    app->setupNewContext();

#ifndef Q_OS_WASM // the browser does the network's encryption
    LOGI() << QString("SSL Info: supported: %1, build: %2, runtime: %3, active backend: %4, available backends: %5")
        .arg(QSslSocket::supportsSsl())
        .arg(QSslSocket::sslLibraryBuildVersionString())
        .arg(QSslSocket::sslLibraryVersionString())
        .arg(QSslSocket::activeBackend())
        .arg(QSslSocket::availableBackends().join(", "));
#endif

    // ====================================================
    // Run main loop
    // ====================================================
    int code = qapp->exec();

    // ====================================================
    // Quit
    // ====================================================

    app->finish();

    delete qapp;

    LOGI() << "Goodbye!! code: " << code;
    return code;
}
