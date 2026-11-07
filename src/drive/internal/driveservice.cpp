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
#include "driveservice.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>

#include "core/driveclient.h"
#include "core/googleauth.h"
#include "core/syncengine.h"

#include "project/inotationproject.h"

#include "translation.h"
#include "log.h"

using namespace mu::drive;
using namespace muse;

// Set at build time (CMake: VS_DRIVE_CLIENT_ID, VS_DRIVE_CLIENT_SECRET, VS_DRIVE_FOLDER_ID).
// The client is of type "Desktop app": Google does not treat its secret as confidential.
#ifndef VS_DRIVE_CLIENT_ID
#define VS_DRIVE_CLIENT_ID ""
#endif
#ifndef VS_DRIVE_CLIENT_SECRET
#define VS_DRIVE_CLIENT_SECRET ""
#endif
#ifndef VS_DRIVE_FOLDER_ID
#define VS_DRIVE_FOLDER_ID ""
#endif

static const QString FOLDER_ID_KEY = QStringLiteral("drive/folderId");
static const QString DEVICE_LABEL_KEY = QStringLiteral("drive/deviceLabel");
static const QString REFRESH_TOKEN_KEY = QStringLiteral("drive/refreshToken");

static constexpr int SYNC_AFTER_START_MS = 2000;
static constexpr int SYNC_AFTER_SAVE_MS = 1500;
static constexpr int SYNC_AFTER_CLOSE_MS = 500;
//! Coming back to the app syncs, at most once in this time
static constexpr int MIN_SECS_BETWEEN_FOREGROUND_SYNCS = 60;

static QString deviceLabel()
{
    const QString saved = QSettings().value(DEVICE_LABEL_KEY).toString();
    if (!saved.isEmpty()) {
        return saved;
    }
#ifdef Q_OS_ANDROID
    return QStringLiteral("Android");
#else
    const QString host = QSysInfo::machineHostName();
    return host.isEmpty() || host == "localhost" ? QStringLiteral("PC") : host;
#endif
}

DriveService::DriveService(const modularity::ContextPtr& iocCtx)
    : Contextable(iocCtx)
{
    m_syncTimer.setSingleShot(true);
    connect(&m_syncTimer, &QTimer::timeout, this, &DriveService::sync);
}

DriveService::~DriveService() = default;

void DriveService::init()
{
    m_nam = new QNetworkAccessManager(this);
    m_tokenStore = std::make_unique<drivesync::SettingsTokenStore>(REFRESH_TOKEN_KEY);

    drivesync::GoogleAuth::ClientConfig config;
    config.clientId = QStringLiteral(VS_DRIVE_CLIENT_ID);
    config.clientSecret = QStringLiteral(VS_DRIVE_CLIENT_SECRET);
    m_auth = new drivesync::GoogleAuth(m_nam, config, m_tokenStore.get(), this);
    m_auth->setDonePageText(muse::qtrc("drive", "Signed in to Google Drive. Go back to the app."));

    m_client = new drivesync::DriveClient(m_nam, m_auth, this);

    connect(m_auth, &drivesync::GoogleAuth::openBrowser, this, [this](const QUrl& url) {
        interactive()->openUrl(url);
    });
    connect(m_auth, &drivesync::GoogleAuth::signedIn, this, [this]() {
        updateState();
        scheduleSync(0);
    });
    connect(m_auth, &drivesync::GoogleAuth::signInFailed, this, [this](const QString& error) {
        LOGW() << "Google sign-in failed: " << error;
        updateState();
        if (error != "Cancelled") {
            interactive()->error(muse::trc("drive", "Google sign-in failed"), IInteractive::Text(error.toStdString()));
        }
    });
    connect(m_auth, &drivesync::GoogleAuth::signedOut, this, &DriveService::updateState);

    if (folderId().isEmpty() && !QString(VS_DRIVE_FOLDER_ID).isEmpty()) {
        QSettings().setValue(FOLDER_ID_KEY, QStringLiteral(VS_DRIVE_FOLDER_ID));
    }

    // Back in the foreground: changes made elsewhere come in
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) {
            return;
        }
        if (!m_lastStart.isValid() || m_lastStart.secsTo(QDateTime::currentDateTime()) >= MIN_SECS_BETWEEN_FOREGROUND_SYNCS) {
            scheduleSync(0);
        }
    });

    globalContext()->currentProjectChanged().onNotify(this, [this]() {
        watchCurrentProject();
        // A score that was closed may have changes from Drive waiting
        scheduleSync(SYNC_AFTER_CLOSE_MS);
    });
    watchCurrentProject();

    updateState();
    scheduleSync(SYNC_AFTER_START_MS);
}

void DriveService::watchCurrentProject()
{
    project::INotationProjectPtr project = globalContext()->currentProject();
    if (!project) {
        return;
    }

    project->saveComplited().onReceive(this, [this](const io::path_t& path, project::SaveMode) {
        const QString root = QDir::cleanPath(rootPath().toQString()) + "/";
        if (QDir::cleanPath(path.toQString()).startsWith(root)) {
            scheduleSync(SYNC_AFTER_SAVE_MS);
        }
    });
}

bool DriveService::isAvailable() const
{
    return m_auth && m_auth->isConfigured();
}

IDriveService::Status DriveService::status() const
{
    return m_status;
}

async::Notification DriveService::statusChanged() const
{
    return m_statusChanged;
}

io::path_t DriveService::rootPath() const
{
    return projectConfiguration()->userProjectsPath() + "/Drive";
}

void DriveService::signIn()
{
    m_auth->signIn();
    updateState();
}

void DriveService::cancelSignIn()
{
    m_auth->cancelSignIn();
    updateState();
}

void DriveService::signOut()
{
    m_auth->signOut();
    updateState();
}

QString DriveService::folderId() const
{
    return QSettings().value(FOLDER_ID_KEY).toString();
}

QString DriveService::folderIdFromLink(const QString& linkOrId)
{
    const QString text = linkOrId.trimmed();
    // https://drive.google.com/drive/folders/<id>, .../drive/u/0/mobile/folders/<id>?..., ...?id=<id>
    static const QRegularExpression folders(QStringLiteral("/folders/([A-Za-z0-9_-]{10,})"));
    static const QRegularExpression idParam(QStringLiteral("[?&]id=([A-Za-z0-9_-]{10,})"));
    static const QRegularExpression bareId(QStringLiteral("^[A-Za-z0-9_-]{10,}$"));

    for (const QRegularExpression* re : { &folders, &idParam }) {
        const QRegularExpressionMatch match = re->match(text);
        if (match.hasMatch()) {
            return match.captured(1);
        }
    }
    return bareId.match(text).hasMatch() ? text : QString();
}

Ret DriveService::setFolder(const QString& linkOrId)
{
    const QString id = folderIdFromLink(linkOrId);
    if (id.isEmpty()) {
        return make_ret(Ret::Code::BadData, muse::trc("drive", "This is not a link to a Google Drive folder."));
    }

    if (id == folderId()) {
        return make_ok();
    }

    // Another folder: what is here belongs to the old one. Kept aside, never deleted
    const QString root = rootPath().toQString();
    if (QDir(root).exists() && !QDir(root).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString aside = root + " (" + QDateTime::currentDateTime().toString("yyyy-MM-dd HHmmss") + ")";
        if (!QDir().rename(root, aside)) {
            return make_ret(Ret::Code::UnknownError, muse::trc("drive", "The scores of the previous folder could not be moved."));
        }
        LOGI() << "Scores of the previous Drive folder moved to " << aside;
    }

    QSettings().setValue(FOLDER_ID_KEY, id);
    m_status.lastSync = QDateTime();
    m_status.conflicts.clear();
    m_status.problems.clear();
    updateState();
    scheduleSync(0);
    return make_ok();
}

bool DriveService::isOpenInApp(const QString& relPath) const
{
    project::INotationProjectPtr project = globalContext()->currentProject();
    if (!project) {
        return false;
    }
    const QString open = QDir::cleanPath(project->path().toQString());
    return open == QDir::cleanPath(rootPath().toQString() + "/" + relPath);
}

void DriveService::scheduleSync(int delayMs)
{
    if (!isAvailable() || !m_auth->isSignedIn() || folderId().isEmpty()) {
        return;
    }
    m_syncTimer.start(delayMs);
}

void DriveService::sync()
{
    if (!isAvailable() || !m_auth->isSignedIn() || folderId().isEmpty()) {
        updateState();
        return;
    }

    if (m_engine) {
        m_syncAgain = true; // a save during the run: one more run after it
        return;
    }

    fileSystem()->makePath(rootPath());

    drivesync::SyncConfig config;
    config.localRoot = rootPath().toQString();
    config.remoteRootId = folderId();
    config.deviceLabel = deviceLabel();
    config.includePatterns = { QStringLiteral("*.mscz") };
    config.isOpenInApp = [this](const QString& relPath) { return isOpenInApp(relPath); };

    m_engine = new drivesync::SyncEngine(m_client, config, this);
    connect(m_engine, &drivesync::SyncEngine::finished, this, &DriveService::onFinished);

    m_lastStart = QDateTime::currentDateTime();
    m_syncAgain = false;
    LOGI() << "Drive sync started: " << config.localRoot;
    m_engine->run();
    updateState();
}

void DriveService::onFinished(const drivesync::SyncReport& report)
{
    m_engine->deleteLater();
    m_engine = nullptr;

    LOGI() << "Drive sync finished: downloaded " << report.downloaded << ", uploaded " << report.uploaded
           << ", deleted here " << report.deletedLocal << ", deleted on Drive " << report.deletedRemote
           << ", errors " << report.errors.size();
    for (const QString& e : report.errors) {
        LOGW() << "Drive sync: " << e;
    }

    m_status.lastSync = QDateTime::currentDateTime();
    m_status.lastSyncOk = report.ok();
    m_status.lastTransfers = report.transfers();
    m_status.conflicts.clear();
    for (const QString& c : report.conflicts) {
        m_status.conflicts << QFileInfo(c).completeBaseName();
    }
    m_status.problems = report.errors;
    for (const QString& s : report.skipped) {
        // Waits for the score to be closed: not a problem for the user
        if (!s.endsWith(QStringLiteral(": open in the app"))) {
            m_status.problems << s;
        }
    }
    if (report.aborted && report.errors.isEmpty()) {
        m_status.problems << muse::qtrc("drive", "Stopped: most of the scores would be deleted. Check the Drive folder.");
    }

    updateState();

    if (m_syncAgain) {
        scheduleSync(0);
    }
}

void DriveService::updateState()
{
    State state = State::Idle;
    if (!isAvailable() || (!m_auth->isSignedIn() && !m_auth->isSigningIn())) {
        state = State::NotSignedIn;
    } else if (m_auth->isSigningIn()) {
        state = State::SigningIn;
    } else if (folderId().isEmpty()) {
        state = State::NoFolder;
    } else if (m_engine) {
        state = State::Syncing;
    }

    m_status.state = state;
    m_statusChanged.notify();
}

RetVal<io::path_t> DriveService::copyToDrive(const io::path_t& scorePath)
{
    const QFileInfo source(scorePath.toQString());
    if (source.suffix().toLower() != "mscz") {
        return RetVal<io::path_t>::make_ret(Ret::Code::NotSupported,
                                            muse::trc("drive", "Only .mscz scores can be shared on Drive."));
    }

    fileSystem()->makePath(rootPath());

    // Same name as on the device; "Name (2).mscz" if the folder has one already
    io::path_t target = rootPath() + "/" + source.fileName();
    for (int i = 2; fileSystem()->exists(target); ++i) {
        target = rootPath() + "/" + QString("%1 (%2).mscz").arg(source.completeBaseName()).arg(i);
    }

    Ret ret = fileSystem()->copy(scorePath, target);
    if (!ret) {
        return RetVal<io::path_t>::make_ret(ret);
    }

    scheduleSync(0);
    return RetVal<io::path_t>::make_ok(target);
}
