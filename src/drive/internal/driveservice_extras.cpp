/*
 * SPDX-License-Identifier: GPL-3.0-only
 * VibeScore - MuseScore Studio for mobile devices
 *
 * Copyright (C) 2026 VibeScore contributors
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
// Diagnostics (logs, crash reports) and updates (GitHub releases of this build)

#include "driveservice.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QTimer>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#endif

#include "crashhandler.h"

#include "translation.h"
#include "log.h"

using namespace mu::drive;
using namespace muse;

// Set at build time (CI: the commit, the time, the GitHub repository)
#ifndef VS_BUILD_ID
#define VS_BUILD_ID "local"
#endif
#ifndef VS_BUILD_TIME
#define VS_BUILD_TIME 0
#endif
#ifndef VS_UPDATE_REPO
#define VS_UPDATE_REPO ""
#endif

static const QString CRASH_SEEN_KEY = QStringLiteral("drive/crashReportsSeenUntil");
static const QString UPDATE_SKIPPED_KEY = QStringLiteral("drive/updateSkipped");
static const QString UPDATE_CHECKED_KEY = QStringLiteral("drive/updateCheckedAt");

static constexpr int CRASH_OFFER_DELAY_MS = 4000;
static constexpr int UPDATE_CHECK_DELAY_MS = 15000;
static constexpr int UPDATE_CHECK_EVERY_SECS = 24 * 60 * 60;

QString DriveService::logsPath() const
{
    return globalConfiguration()->userAppDataPath().toQString() + "/logs";
}

void DriveService::initExtras()
{
    CrashHandler::install(logsPath());

    QTimer::singleShot(CRASH_OFFER_DELAY_MS, this, [this]() { offerCrashReport(); });

    QTimer::singleShot(UPDATE_CHECK_DELAY_MS, this, [this]() {
        const qint64 last = QSettings().value(UPDATE_CHECKED_KEY).toLongLong();
        if (QDateTime::currentSecsSinceEpoch() - last >= UPDATE_CHECK_EVERY_SECS) {
            checkForUpdates(false);
        }
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Diagnostics

static QFileInfoList newestFiles(const QString& dir, const QString& pattern, int count)
{
    QFileInfoList files = QDir(dir).entryInfoList({ pattern }, QDir::Files, QDir::Time);
    files.erase(std::remove_if(files.begin(), files.end(), [](const QFileInfo& f) { return f.size() == 0; }), files.end());
    return files.mid(0, count);
}

void DriveService::offerCrashReport()
{
    const QFileInfoList crashes = newestFiles(logsPath(), QStringLiteral("crash_*.txt"), 1);
    if (crashes.isEmpty()) {
        return;
    }

    QSettings settings;
    const qint64 seenUntil = settings.value(CRASH_SEEN_KEY).toLongLong();
    const qint64 newest = crashes.first().lastModified().toSecsSinceEpoch();
    if (newest <= seenUntil) {
        return;
    }
    settings.setValue(CRASH_SEEN_KEY, newest);

    IInteractive::ButtonData no(int(IInteractive::Button::CustomButton) + 1, muse::trc("drive", "No"));
    no.role = IInteractive::ButtonRole::RejectRole;
    IInteractive::ButtonData send(int(IInteractive::Button::CustomButton) + 2, muse::trc("drive", "Send"), true);
    send.role = IInteractive::ButtonRole::AcceptRole;

    interactive()->question(muse::trc("drive", "The app closed because of an error"),
                            IInteractive::Text(muse::trc("drive", "Send the report? It has the app's log, with the names "
                                                                  "of the scores opened, and no score content.")),
                            { no, send }, send.btn)
    .onResolve(this, [this, sendBtn = send.btn](const IInteractive::Result& result) {
        if (result.isButton(sendBtn)) {
            shareDiagnostics();
        }
    });
}

void DriveService::shareDiagnostics()
{
    // The latest crash reports and the logs of this session and the previous ones
    QStringList paths;
    for (const QFileInfo& f : newestFiles(logsPath(), QStringLiteral("crash_*.txt"), 3)) {
        paths << f.absoluteFilePath();
    }
    for (const QFileInfo& f : newestFiles(logsPath(), QStringLiteral("*.log"), 3)) {
        paths << f.absoluteFilePath();
    }

    if (paths.isEmpty()) {
        interactive()->info(muse::trc("drive", "Diagnostics"), IInteractive::Text(muse::trc("drive", "There are no logs yet.")));
        return;
    }

#ifdef Q_OS_ANDROID
    QJniEnvironment env;
    jobjectArray jpaths = env->NewObjectArray(static_cast<jsize>(paths.size()), env.findClass("java/lang/String"), nullptr);
    for (qsizetype i = 0; i < paths.size(); ++i) {
        const QJniObject path = QJniObject::fromString(paths.at(i));
        env->SetObjectArrayElement(jpaths, static_cast<jsize>(i), path.object<jstring>());
    }
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject title = QJniObject::fromString(muse::qtrc("drive", "Send diagnostics"));
    QJniObject::callStaticMethod<void>("io/github/m1omg/vibescore/VibeScoreShare", "shareFiles",
                                       "(Landroid/content/Context;[Ljava/lang/String;Ljava/lang/String;)V",
                                       context.object(), jpaths, title.object<jstring>());
    if (env.checkAndClearExceptions()) {
        LOGE() << "Failed to share the diagnostics";
    }
#else
    interactive()->revealInFileBrowser(paths.first());
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// Updates: the newest GitHub release of the repository this build comes from

void DriveService::checkForUpdates(bool userAsked)
{
    const QString repo = QStringLiteral(VS_UPDATE_REPO);
    if (repo.isEmpty()) {
        if (userAsked) {
            interactive()->info(muse::trc("drive", "Updates"),
                                IInteractive::Text(muse::trc("drive", "This build does not look for updates.")));
        }
        return;
    }

    QNetworkRequest request(QUrl("https://api.github.com/repos/" + repo + "/releases/latest"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "VibeScore-Drive");
    request.setTransferTimeout(30000);

    QNetworkReply* reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, userAsked]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            LOGW() << "Update check failed: " << reply->errorString().toStdString();
            if (userAsked) {
                interactive()->error(muse::trc("drive", "Updates"), IInteractive::Text(reply->errorString().toStdString()));
            }
            return;
        }
        QSettings().setValue(UPDATE_CHECKED_KEY, QDateTime::currentSecsSinceEpoch());

        const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = release.value("tag_name").toString();
        const QDateTime published = QDateTime::fromString(release.value("published_at").toString(), Qt::ISODate);

        // This build's own release, or an older one: nothing to do
        const QString buildId = QStringLiteral(VS_BUILD_ID);
        const bool isThisBuild = !buildId.isEmpty() && buildId != "local" && tag.endsWith(buildId.left(7));
        const bool newer = published.isValid() && published.toSecsSinceEpoch() > qint64(VS_BUILD_TIME) + 60;
        if (tag.isEmpty() || isThisBuild || !newer) {
            if (userAsked) {
                interactive()->info(muse::trc("drive", "Updates"),
                                    IInteractive::Text(muse::trc("drive", "You have the latest version.")));
            }
            return;
        }

        if (!userAsked && QSettings().value(UPDATE_SKIPPED_KEY).toString() == tag) {
            return;
        }

        // The file for this platform
#ifdef Q_OS_ANDROID
        const QStringList suffixes { ".apk" };
#else
        const QStringList suffixes { ".msi", ".7z" };
#endif
        QString download;
        for (const QString& suffix : suffixes) {
            for (const QJsonValue& asset : release.value("assets").toArray()) {
                const QString name = asset.toObject().value("name").toString();
                if (download.isEmpty() && name.endsWith(suffix)) {
                    download = asset.toObject().value("browser_download_url").toString();
                }
            }
        }
        if (download.isEmpty()) {
            download = release.value("html_url").toString();
        }

        IInteractive::ButtonData later(int(IInteractive::Button::CustomButton) + 1, muse::trc("drive", "Later"));
        later.role = IInteractive::ButtonRole::RejectRole;
        IInteractive::ButtonData get(int(IInteractive::Button::CustomButton) + 2, muse::trc("drive", "Download"), true);
        get.role = IInteractive::ButtonRole::AcceptRole;

        //: %1 is a date
        const QString text = muse::qtrc("drive", "A new version from %1 is available. Download it? Then open the "
                                                 "downloaded file to install it over this one.")
                             .arg(QLocale().toString(published.toLocalTime().date(), QLocale::ShortFormat));
        interactive()->question(muse::trc("drive", "Update available"), IInteractive::Text(text.toStdString()),
                                { later, get }, get.btn)
        .onResolve(this, [this, tag, download, getBtn = get.btn](const IInteractive::Result& result) {
            if (result.isButton(getBtn)) {
                interactive()->openUrl(QUrl(download));
            } else {
                QSettings().setValue(UPDATE_SKIPPED_KEY, tag);
            }
        });
    });
}
