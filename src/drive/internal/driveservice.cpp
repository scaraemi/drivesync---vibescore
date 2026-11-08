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

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>

#include "core/driveclient.h"
#include "core/googleauth.h"
#include "core/syncengine.h"
#include "core/syncmanifest.h"

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
static const QString DEVICE_ID_KEY = QStringLiteral("drive/deviceId");
static const QString REFRESH_TOKEN_KEY = QStringLiteral("drive/refreshToken");

static constexpr int SYNC_AFTER_START_MS = 2000;
static constexpr int SYNC_AFTER_SAVE_MS = 1500;
static constexpr int SYNC_AFTER_CLOSE_MS = 500;
//! Coming back to the app syncs, at most once in this time
static constexpr int MIN_SECS_BETWEEN_FOREGROUND_SYNCS = 60;
//! Another process (another MuseScore window) is syncing: try again after this
static constexpr int SYNC_BUSY_RETRY_MS = 30 * 1000;

//! While the app is in the foreground: the open score is checked every minute, the whole folder every 5
static constexpr int POLL_MS = 60 * 1000;
static constexpr int POLL_TICKS_PER_SYNC = 5;

//! A lock lasts this long unless renewed; renewed while the changes are not saved
static constexpr int LOCK_TTL_SECS = 2 * 60 * 60;
static constexpr int LOCK_RENEW_MS = 30 * 60 * 1000;

static const QRegularExpression LOCK_TAG(QStringLiteral("\\[vs:([0-9A-Za-z-]+):(\\d+)\\]"));

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

static QString humanLockReason(const QString& reason)
{
    return reason.left(reason.indexOf(" [vs:")).trimmed();
}

DriveService::DriveService(const modularity::ContextPtr& iocCtx)
    : Contextable(iocCtx)
{
    m_syncTimer.setSingleShot(true);
    connect(&m_syncTimer, &QTimer::timeout, this, &DriveService::sync);

    m_pollTimer.setInterval(POLL_MS);
    connect(&m_pollTimer, &QTimer::timeout, this, &DriveService::onPollTimer);

    m_lockRenewTimer.setInterval(LOCK_RENEW_MS);
    connect(&m_lockRenewTimer, &QTimer::timeout, this, &DriveService::takeLock);
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
        LOGW() << "Google sign-in failed: " << error.toStdString();
        updateState();
        if (error != "Cancelled") {
            interactive()->error(muse::trc("drive", "Google sign-in failed"), IInteractive::Text(error.toStdString()));
        }
    });
    connect(m_auth, &drivesync::GoogleAuth::signedOut, this, &DriveService::updateState);

    if (folderId().isEmpty() && !QString(VS_DRIVE_FOLDER_ID).isEmpty()) {
        QSettings().setValue(FOLDER_ID_KEY, QStringLiteral(VS_DRIVE_FOLDER_ID));
    }

    // In the foreground: changes made elsewhere come in, and the open score is watched
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) {
            m_pollTimer.stop();
            return;
        }
        m_pollTimer.start();
        if (!m_lastStart.isValid() || m_lastStart.secsTo(QDateTime::currentDateTime()) >= MIN_SECS_BETWEEN_FOREGROUND_SYNCS) {
            scheduleSync(0);
        }
        checkOpenScore(false);
    });
    m_pollTimer.start();

    globalContext()->currentProjectChanged().onNotify(this, [this]() {
        onCurrentProjectChanged();
    });

    loadCatalog();
    onCurrentProjectChanged();
    updateState();
    scheduleSync(SYNC_AFTER_START_MS);
}

void DriveService::deinit()
{
    // The lock of unsaved changes goes with the app; if this does not get through, it expires
    if (m_open.lockHeld && !m_open.fileId.isEmpty() && m_client) {
        m_client->setLocked(m_open.fileId, false, QString(), [](drivesync::Result<drivesync::DriveFile>) {});
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Paths

io::path_t DriveService::rootPath() const
{
    return projectConfiguration()->userProjectsPath() + "/Drive";
}

QString DriveService::absPath(const QString& relPath) const
{
    return QDir::cleanPath(rootPath().toQString()) + "/" + relPath;
}

QString DriveService::relPathOf(const io::path_t& path) const
{
    const QString root = QDir::cleanPath(rootPath().toQString()) + "/";
    const QString p = QDir::cleanPath(path.toQString());
    return p.startsWith(root) ? p.mid(root.size()) : QString();
}

QString DriveService::openMarkerPath(const QString& relPath) const
{
    const QByteArray key = QCryptographicHash::hash(relPath.toUtf8(), QCryptographicHash::Sha1).toHex();
    return rootPath().toQString() + "/.drivesync/open/" + QString::fromLatin1(key);
}

bool DriveService::isOpenInApp(const QString& relPath) const
{
    if (!m_open.relPath.isEmpty() && m_open.relPath == relPath) {
        return true;
    }
    // Open in another process of the app (desktop: one per score)
    return HeartbeatFile::isHeldByAnyone(openMarkerPath(relPath));
}

// ---------------------------------------------------------------------------------------------------------------------
// State

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
        LOGI() << "Scores of the previous Drive folder moved to " << aside.toStdString();
    }

    QSettings().setValue(FOLDER_ID_KEY, id);
    m_status.lastSync = QDateTime();
    m_status.conflicts.clear();
    m_status.problems.clear();
    m_items.clear();
    m_fileIds.clear();
    m_itemsChanged.notify();
    updateState();
    scheduleSync(0);
    return make_ok();
}

// ---------------------------------------------------------------------------------------------------------------------
// Sync

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

    // One sync at a time over the folder, across the app's processes
    if (!m_syncLock) {
        m_syncLock = std::make_unique<HeartbeatFile>(rootPath().toQString() + "/.drivesync/sync.lock");
    }
    if (!m_syncLock->acquire()) {
        LOGI() << "Drive sync: another window is syncing, trying again later";
        m_syncTimer.start(SYNC_BUSY_RETRY_MS);
        return;
    }

    drivesync::SyncConfig config;
    config.localRoot = rootPath().toQString();
    config.remoteRootId = folderId();
    config.deviceLabel = deviceLabel();
    config.includePatterns = { QStringLiteral("*.mscz") };
    config.isOpenInApp = [this](const QString& relPath) { return isOpenInApp(relPath); };
    config.onDemand = true;
    config.wanted = [this](const QString& relPath) { return m_wanted.contains(relPath); };
    config.canLiftLock = [this](const drivesync::DriveFile& file) { return isOwnLock(file); };

    m_engine = new drivesync::SyncEngine(m_client, config, this);
    connect(m_engine, &drivesync::SyncEngine::finished, this, &DriveService::onFinished);

    m_lastStart = QDateTime::currentDateTime();
    m_pollTicks = 0;
    m_syncAgain = false;
    LOGI() << "Drive sync started: " << config.localRoot.toStdString();
    m_engine->run();
    updateState();
}

void DriveService::onFinished(const drivesync::SyncReport& report)
{
    if (!report.aborted || !m_engine->remoteFiles().isEmpty()) {
        updateCatalog(*m_engine);
    }
    m_engine->deleteLater();
    m_engine = nullptr;
    m_syncLock->release();

    LOGI() << "Drive sync finished: downloaded " << report.downloaded << ", uploaded " << report.uploaded
           << ", deleted here " << report.deletedLocal << ", deleted on Drive " << report.deletedRemote
           << ", errors " << report.errors.size();
    for (const QString& e : report.errors) {
        LOGW() << "Drive sync: " << e.toStdString();
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
        // Waiting for the score to be closed, or for another device to save it: not a problem
        if (!s.endsWith(QStringLiteral(": open in the app")) && !s.contains(QStringLiteral(": locked on Drive"))) {
            m_status.problems << s;
        }
    }
    if (report.aborted && report.errors.isEmpty()) {
        m_status.problems << muse::qtrc("drive", "Stopped: most of the scores would be deleted. Check the Drive folder.");
    }

    // The uploads lifted this device's lock: the changes are on Drive now
    if (!m_open.relPath.isEmpty() && m_fileIds.contains(m_open.relPath)) {
        m_open.fileId = m_fileIds.value(m_open.relPath);
        const auto item = m_items.constFind(m_open.relPath);
        if (item != m_items.cend() && !item->lockedHere) {
            m_open.lockHeld = false;
            m_lockRenewTimer.stop();
        }
        drivesync::SyncManifest manifest;
        manifest.load(rootPath().toQString() + "/.drivesync/manifest.json");
        const QString revision = manifest.entries.value(m_open.relPath).baseRevision;
        if (!revision.isEmpty()) {
            m_open.knownRevision = revision;
        }
    }

    // Downloaded to be opened
    for (auto it = m_wanted.begin(); it != m_wanted.end();) {
        it = QFileInfo::exists(absPath(*it)) ? m_wanted.erase(it) : std::next(it);
    }
    const QString toOpen = !m_openAfterSync.isEmpty() ? m_openAfterSync : QString();
    m_openAfterSync.clear();
    m_status.fetching.clear();
    if (!toOpen.isEmpty()) {
        m_wanted.remove(toOpen);
        if (QFileInfo::exists(absPath(toOpen))) {
            dispatcher()->dispatch("file-open", actions::ActionData::make_arg2<QUrl, QString>(QUrl::fromLocalFile(absPath(toOpen)),
                                                                                              QString()));
        } else {
            interactive()->error(muse::trc("drive", "The score could not be downloaded"),
                                 IInteractive::Text((report.errors.isEmpty() ? toOpen : report.errors.join("\n")).toStdString()));
        }
    }

    updateState();

    if (m_syncAgain || !m_wanted.isEmpty()) {
        scheduleSync(0);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Catalog

static QString catalogPath(const QString& root)
{
    return root + "/.drivesync/catalog.json";
}

void DriveService::updateCatalog(const drivesync::SyncEngine& engine)
{
    m_items.clear();
    m_fileIds.clear();

    for (const QString& folder : engine.remoteFolders()) {
        Item item;
        item.path = folder;
        item.isFolder = true;
        m_items.insert(folder, item);
    }

    const auto& folderIds = engine.remoteFolderIds();
    for (auto it = folderIds.cbegin(); it != folderIds.cend(); ++it) {
        if (!it.key().isEmpty()) {
            m_fileIds.insert(it.key(), it.value());
        }
    }

    const auto& remote = engine.remoteFiles();
    for (auto it = remote.cbegin(); it != remote.cend(); ++it) {
        const drivesync::DriveFile& f = it.value();
        Item item;
        item.path = it.key();
        item.size = f.size;
        item.modified = f.modifiedTime.toLocalTime();
        item.modifiedBy = f.lastModifiedByMe ? muse::qtrc("drive", "you") : f.lastModifyingUser;
        if (f.locked) {
            item.lockedBy = f.lockedBy.isEmpty() ? muse::qtrc("drive", "someone") : f.lockedBy;
            item.lockReason = humanLockReason(f.lockReason);
            item.lockedSince = f.lockTime.toLocalTime();
            item.lockedHere = isOwnLock(f);
            item.lockStale = isStaleLock(f);
        }
        m_items.insert(item.path, item);
        m_fileIds.insert(item.path, f.id);
    }

    refreshLocalState();
    saveCatalog();
}

void DriveService::refreshLocalState()
{
    // What is on this device: downloaded scores, and new ones not uploaded yet
    for (Item& item : m_items) {
        if (!item.isFolder) {
            item.onDevice = false;
            item.notUploaded = false;
        }
    }

    const QString root = QDir::cleanPath(rootPath().toQString());
    QDirIterator it(root, { QStringLiteral("*.mscz") }, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString rel = it.next().mid(root.size() + 1);
        if (rel.startsWith('.') || rel.contains(QStringLiteral("/."))) {
            continue;
        }
        auto item = m_items.find(rel);
        if (item == m_items.end()) {
            Item local;
            local.path = rel;
            local.size = QFileInfo(it.filePath()).size();
            local.modified = QFileInfo(it.filePath()).lastModified();
            local.notUploaded = true;
            item = m_items.insert(rel, local);
            // Its folders, if they are new too
            for (int slash = rel.lastIndexOf('/'); slash > 0; slash = rel.lastIndexOf('/', slash - 1)) {
                const QString dir = rel.left(slash);
                if (!m_items.contains(dir)) {
                    Item folder;
                    folder.path = dir;
                    folder.isFolder = true;
                    m_items.insert(dir, folder);
                }
            }
        }
        item->onDevice = true;
    }

    m_itemsChanged.notify();
}

void DriveService::loadCatalog()
{
    QFile file(catalogPath(QDir::cleanPath(rootPath().toQString())));
    if (!file.open(QIODevice::ReadOnly)) {
        refreshLocalState();
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value("folderId").toString() != folderId()) {
        refreshLocalState();
        return;
    }

    for (const QJsonValue& v : root.value("items").toArray()) {
        const QJsonObject o = v.toObject();
        Item item;
        item.path = o.value("path").toString();
        item.isFolder = o.value("folder").toBool();
        item.size = o.value("size").toInteger();
        item.modified = QDateTime::fromString(o.value("modified").toString(), Qt::ISODate);
        item.modifiedBy = o.value("modifiedBy").toString();
        item.lockedBy = o.value("lockedBy").toString();
        item.lockReason = o.value("lockReason").toString();
        item.lockedSince = QDateTime::fromString(o.value("lockedSince").toString(), Qt::ISODate);
        item.lockedHere = o.value("lockedHere").toBool();
        item.lockStale = o.value("lockStale").toBool();
        m_items.insert(item.path, item);
        if (o.contains("id")) {
            m_fileIds.insert(item.path, o.value("id").toString());
        }
    }
    refreshLocalState();
}

void DriveService::saveCatalog() const
{
    QJsonArray items;
    for (const Item& item : m_items) {
        if (item.notUploaded) {
            continue; // found again by looking at the folder
        }
        QJsonObject o {
            { "path", item.path },
            { "folder", item.isFolder },
            { "size", item.size },
            { "modified", item.modified.toString(Qt::ISODate) },
            { "modifiedBy", item.modifiedBy },
        };
        if (!item.lockedBy.isEmpty()) {
            o.insert("lockedBy", item.lockedBy);
            o.insert("lockReason", item.lockReason);
            o.insert("lockedSince", item.lockedSince.toString(Qt::ISODate));
            o.insert("lockedHere", item.lockedHere);
            o.insert("lockStale", item.lockStale);
        }
        if (m_fileIds.contains(item.path)) {
            o.insert("id", m_fileIds.value(item.path));
        }
        items.append(o);
    }

    const QString root = QDir::cleanPath(rootPath().toQString());
    QDir().mkpath(root + "/.drivesync");
    QSaveFile file(catalogPath(root));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject { { "folderId", folderId() }, { "items", items } }).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

QList<IDriveService::Item> DriveService::items() const
{
    return m_items.values();
}

async::Notification DriveService::itemsChanged() const
{
    return m_itemsChanged;
}

QString DriveService::fileIdOf(const QString& relPath) const
{
    return m_fileIds.value(relPath);
}

// ---------------------------------------------------------------------------------------------------------------------
// Actions on the scores of the folder

void DriveService::openScore(const QString& relPath)
{
    if (QFileInfo::exists(absPath(relPath))) {
        dispatcher()->dispatch("file-open", actions::ActionData::make_arg2<QUrl, QString>(QUrl::fromLocalFile(absPath(relPath)),
                                                                                          QString()));
        return;
    }

    // On demand: downloaded by the next run, then opened
    m_wanted.insert(relPath);
    m_openAfterSync = relPath;
    m_status.fetching = relPath;
    updateState();
    if (m_engine) {
        m_syncAgain = true;
    } else {
        sync();
    }
}

Ret DriveService::removeFromDevice(const QString& relPath)
{
    if (isOpenInApp(relPath)) {
        return make_ret(Ret::Code::UnknownError, muse::trc("drive", "Close the score first."));
    }

    const auto item = m_items.constFind(relPath);
    if (item != m_items.cend() && item->notUploaded) {
        return make_ret(Ret::Code::UnknownError, muse::trc("drive", "This score is not on Drive yet."));
    }

    // Not during a sync, in this process or another
    if (m_engine) {
        return make_ret(Ret::Code::UnknownError, muse::trc("drive", "Syncing: try again in a moment."));
    }
    if (!m_syncLock) {
        m_syncLock = std::make_unique<HeartbeatFile>(rootPath().toQString() + "/.drivesync/sync.lock");
    }
    if (!m_syncLock->acquire()) {
        return make_ret(Ret::Code::UnknownError, muse::trc("drive", "Syncing: try again in a moment."));
    }

    QString error;
    const bool ok = drivesync::SyncEngine::removeFromDevice(rootPath().toQString(), relPath, &error);
    m_syncLock->release();
    if (!ok) {
        if (error.contains("not on Drive yet")) {
            scheduleSync(0);
            return make_ret(Ret::Code::UnknownError,
                            muse::trc("drive", "This score has changes that are not on Drive yet: they are being uploaded."));
        }
        return make_ret(Ret::Code::UnknownError, error.toStdString());
    }

    refreshLocalState();
    return make_ok();
}

void DriveService::deleteFromDrive(const QString& relPath)
{
    const QString fileId = fileIdOf(relPath);
    const auto item = m_items.constFind(relPath);

    if (fileId.isEmpty()) {
        // Only here: deleting the file is enough
        if (!isOpenInApp(relPath)) {
            QFile::remove(absPath(relPath));
        }
        refreshLocalState();
        return;
    }

    if (item != m_items.cend() && !item->lockedBy.isEmpty() && !item->lockedHere && !item->lockStale) {
        interactive()->error(muse::trc("drive", "This score is being edited"),
                             muse::qtrc("drive", "%1 has unsaved changes on it.").arg(item->lockedBy).toStdString());
        return;
    }

    m_client->trash(fileId, [this, relPath](drivesync::Result<drivesync::DriveFile> result) {
        if (!result.ok()) {
            interactive()->error(muse::trc("drive", "The score could not be deleted"), result.error.toString().toStdString());
            return;
        }
        // Off the device too, unless it has changes made here (those would go back to Drive as a new score)
        if (!isOpenInApp(relPath)) {
            drivesync::SyncEngine::removeFromDevice(rootPath().toQString(), relPath);
        }
        m_items.remove(relPath);
        m_fileIds.remove(relPath);
        saveCatalog();
        refreshLocalState();
        scheduleSync(0);
    });
}

void DriveService::unlock(const QString& relPath)
{
    const QString fileId = fileIdOf(relPath);
    if (fileId.isEmpty()) {
        return;
    }

    m_client->setLocked(fileId, false, QString(), [this, relPath](drivesync::Result<drivesync::DriveFile> result) {
        if (!result.ok()) {
            interactive()->error(muse::trc("drive", "The score could not be unlocked"), result.error.toString().toStdString());
            return;
        }
        auto item = m_items.find(relPath);
        if (item != m_items.end()) {
            item->lockedBy.clear();
            item->lockReason.clear();
            item->lockedHere = false;
            item->lockStale = false;
            m_itemsChanged.notify();
        }
        if (relPath == m_open.relPath) {
            m_open.lockHeld = false;
            m_lockRenewTimer.stop();
        }
    });
}

RetVal<io::path_t> DriveService::copyToDrive(const io::path_t& scorePath)
{
    const QFileInfo source(scorePath.toQString());
    if (source.suffix().toLower() != "mscz") {
        return RetVal<io::path_t>::make_ret(Ret::Code::NotSupported,
                                            muse::trc("drive", "Only .mscz scores can be shared on Drive."));
    }

    fileSystem()->makePath(rootPath());

    // Same name as on the device; "Name (2).mscz" if the folder has one already (here or only on Drive)
    QString name = source.fileName();
    for (int i = 2; QFileInfo::exists(absPath(name)) || m_items.contains(name); ++i) {
        name = QString("%1 (%2).mscz").arg(source.completeBaseName()).arg(i);
    }
    const io::path_t target = absPath(name);

    Ret ret = fileSystem()->copy(scorePath, target);
    if (!ret) {
        return RetVal<io::path_t>::make_ret(ret);
    }

    refreshLocalState();
    scheduleSync(0);
    return RetVal<io::path_t>::make_ok(target);
}

// ---------------------------------------------------------------------------------------------------------------------
// The open score: "being edited" lock, changes made on other devices

QString DriveService::deviceId() const
{
    QSettings settings;
    QString id = settings.value(DEVICE_ID_KEY).toString();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(DEVICE_ID_KEY, id);
    }
    return id;
}

QString DriveService::lockReasonText() const
{
    const qint64 until = QDateTime::currentSecsSinceEpoch() + LOCK_TTL_SECS;
    //: The reason of the lock of a score on Google Drive. %1 is a device, e.g. "Tablet" or a computer name
    return muse::qtrc("drive", "Unsaved changes on %1").arg(deviceLabel())
           + QStringLiteral(" [vs:%1:%2]").arg(deviceId()).arg(until);
}

bool DriveService::isOwnLock(const drivesync::DriveFile& file) const
{
    const QRegularExpressionMatch match = LOCK_TAG.match(file.lockReason);
    return file.locked && match.hasMatch() && match.captured(1) == deviceId();
}

bool DriveService::isStaleLock(const drivesync::DriveFile& file) const
{
    // Only the app's own locks expire; a lock set by hand on Drive stays until lifted
    const QRegularExpressionMatch match = LOCK_TAG.match(file.lockReason);
    return file.locked && match.hasMatch() && match.captured(2).toLongLong() < QDateTime::currentSecsSinceEpoch();
}

void DriveService::markOpenProject()
{
    // Tells the other processes of the app (desktop: one per open score) that this score is open here
    m_openMarker.reset();
    if (!m_open.relPath.isEmpty()) {
        m_openMarker = std::make_unique<HeartbeatFile>(openMarkerPath(m_open.relPath));
        m_openMarker->acquire();
    }
}

void DriveService::onCurrentProjectChanged()
{
    // The score that was open: its lock goes, and changes from Drive can come in
    const OpenScore previous = m_open;
    if (previous.lockHeld && !previous.fileId.isEmpty()) {
        // Its changes were saved (or dropped) on closing: uploaded by the sync below
        releaseLock(previous.fileId);
    }
    m_lockRenewTimer.stop();
    m_open = OpenScore();

    project::INotationProjectPtr project = globalContext()->currentProject();
    if (project) {
        m_open.relPath = relPathOf(project->path());
        if (!m_open.relPath.isEmpty()) {
            m_open.fileId = fileIdOf(m_open.relPath);
            drivesync::SyncManifest manifest;
            manifest.load(rootPath().toQString() + "/.drivesync/manifest.json");
            const drivesync::ManifestEntry entry = manifest.entries.value(m_open.relPath);
            m_open.knownRevision = entry.baseRevision;
            if (m_open.fileId.isEmpty()) {
                m_open.fileId = entry.fileId;
            }
        }

        project->saveComplited().onReceive(this, [this](const io::path_t& path, project::SaveMode) {
            const QString rel = relPathOf(path);
            if (rel != m_open.relPath) {
                // "Save as" into or out of the folder
                m_open.relPath = rel;
                m_open.fileId = fileIdOf(rel);
                markOpenProject();
            }
            if (!rel.isEmpty()) {
                scheduleSync(SYNC_AFTER_SAVE_MS); // uploads, and lifts this device's lock
            }
        });

        project->needSave().notification.onNotify(this, [this]() {
            onOpenScoreModified();
        });
    }

    markOpenProject();

    if (!previous.relPath.isEmpty() || !m_open.relPath.isEmpty()) {
        scheduleSync(SYNC_AFTER_CLOSE_MS);
    }

    if (!m_reopenAfterClose.isEmpty() && m_open.relPath.isEmpty()) {
        // Closed to be reloaded: the new version comes with the next run, then the score opens again
        m_openAfterSync = m_reopenAfterClose;
        m_wanted.insert(m_reopenAfterClose);
        m_reopenAfterClose.clear();
        scheduleSync(0);
    }

    checkOpenScore(true);
}

void DriveService::checkOpenScore(bool opening)
{
    if (m_open.relPath.isEmpty() || m_open.fileId.isEmpty() || !m_auth || !m_auth->isSignedIn()) {
        return;
    }

    const QString rel = m_open.relPath;
    m_client->getFile(m_open.fileId, [this, rel, opening](drivesync::Result<drivesync::DriveFile> result) {
        if (!result.ok() || rel != m_open.relPath) {
            return;
        }
        const drivesync::DriveFile file = *result.value;
        const QString name = QFileInfo(rel).completeBaseName();

        // Someone else is editing it
        if (file.locked && !isOwnLock(file) && !isStaleLock(file) && opening && !m_open.warnedLocked) {
            m_open.warnedLocked = true;
            const QString since = QLocale().toString(file.lockTime.toLocalTime().time(), QLocale::ShortFormat);
            interactive()->info(muse::trc("drive", "This score is being edited"),
                                muse::qtrc("drive", "%1 has had unsaved changes on “%2” since %3 (%4). "
                                                    "You can look at it; if you change it too, your version will be kept "
                                                    "as a separate copy.")
                                .arg(file.lockedBy, name, since, humanLockReason(file.lockReason)).toStdString());
        }

        // A new version on Drive
        if (file.headRevisionId.isEmpty() || m_open.knownRevision.isEmpty() || file.headRevisionId == m_open.knownRevision) {
            return;
        }
        m_open.knownRevision = file.headRevisionId;

        const QString who = file.lastModifiedByMe ? muse::qtrc("drive", "You (on another device)") : file.lastModifyingUser;
        const QString when = QLocale().toString(file.modifiedTime.toLocalTime().time(), QLocale::ShortFormat);
        project::INotationProjectPtr project = globalContext()->currentProject();
        const bool unsaved = project && project->needSave().val;

        if (unsaved) {
            interactive()->info(muse::trc("drive", "A newer version is on Drive"),
                                muse::qtrc("drive", "%1 saved “%2” at %3. When you save, your version will be kept as a "
                                                    "separate copy next to theirs.").arg(who, name, when).toStdString());
            return;
        }

        IInteractive::ButtonData later(int(IInteractive::Button::CustomButton) + 1, muse::trc("drive", "Later"));
        later.role = IInteractive::ButtonRole::RejectRole;
        IInteractive::ButtonData reload(int(IInteractive::Button::CustomButton) + 2, muse::trc("drive", "Reload"), true);
        reload.role = IInteractive::ButtonRole::AcceptRole;

        interactive()->question(muse::trc("drive", "A newer version is on Drive"),
                                IInteractive::Text(muse::qtrc("drive", "%1 saved “%2” at %3. Reload it?")
                                                   .arg(who, name, when).toStdString()),
                                { later, reload }, reload.btn)
        .onResolve(this, [this, rel, reloadBtn = reload.btn](const IInteractive::Result& answer) {
            if (answer.isButton(reloadBtn) && rel == m_open.relPath) {
                reloadOpenScore();
            }
        });
    });
}

void DriveService::reloadOpenScore()
{
    project::INotationProjectPtr project = globalContext()->currentProject();
    if (!project || m_open.relPath.isEmpty() || project->needSave().val) {
        return;
    }
    m_reopenAfterClose = m_open.relPath;
    dispatcher()->dispatch("file-close");
}

void DriveService::onOpenScoreModified()
{
    project::INotationProjectPtr project = globalContext()->currentProject();
    if (!project || m_open.relPath.isEmpty() || m_open.fileId.isEmpty() || m_open.lockHeld) {
        return;
    }
    if (project->needSave().val) {
        takeLock();
    }
}

void DriveService::takeLock()
{
    if (m_open.fileId.isEmpty() || !m_auth || !m_auth->isSignedIn()) {
        return;
    }

    // Unsaved changes here: the others see the score as being edited
    const QString fileId = m_open.fileId;
    m_client->getFile(fileId, [this, fileId](drivesync::Result<drivesync::DriveFile> current) {
        if (!current.ok() || fileId != m_open.fileId) {
            return;
        }
        const drivesync::DriveFile file = *current.value;
        if (file.locked && !isOwnLock(file) && !isStaleLock(file)) {
            if (!m_open.warnedLocked) {
                m_open.warnedLocked = true;
                interactive()->info(muse::trc("drive", "This score is being edited"),
                                    muse::qtrc("drive", "%1 is editing it too (%2). When you save, your version will be kept "
                                                        "as a separate copy.")
                                    .arg(file.lockedBy, humanLockReason(file.lockReason)).toStdString());
            }
            return;
        }

        m_client->setLocked(fileId, true, lockReasonText(), [this, fileId](drivesync::Result<drivesync::DriveFile> locked) {
            if (!locked.ok() || fileId != m_open.fileId) {
                if (!locked.ok()) {
                    LOGW() << "Drive: cannot lock: " << locked.error.toString().toStdString();
                }
                return;
            }
            m_open.lockHeld = true;
            m_lockRenewTimer.start();
            auto item = m_items.find(m_open.relPath);
            if (item != m_items.end()) {
                item->lockedBy = locked.value->lockedBy;
                item->lockReason = humanLockReason(locked.value->lockReason);
                item->lockedSince = QDateTime::currentDateTime();
                item->lockedHere = true;
                item->lockStale = false;
                m_itemsChanged.notify();
            }
        });
    });
}

void DriveService::releaseLock(const QString& fileId)
{
    m_client->getFile(fileId, [this, fileId](drivesync::Result<drivesync::DriveFile> current) {
        if (!current.ok() || !isOwnLock(*current.value)) {
            return;
        }
        m_client->setLocked(fileId, false, QString(), [this](drivesync::Result<drivesync::DriveFile>) {
            scheduleSync(SYNC_AFTER_CLOSE_MS);
        });
    });
}

void DriveService::onPollTimer()
{
    if (!isAvailable() || !m_auth->isSignedIn() || folderId().isEmpty()) {
        return;
    }
    if (++m_pollTicks >= POLL_TICKS_PER_SYNC) {
        scheduleSync(0); // the folder: new scores, locks
        return;
    }
    checkOpenScore(false);
}

// ---------------------------------------------------------------------------------------------------------------------
// Folders, renames, moves: on Drive first, then the local copies follow

static bool isValidName(const QString& name)
{
    return !name.trimmed().isEmpty() && !name.contains('/') && !name.contains('\\') && !name.startsWith('.');
}

static QString parentOf(const QString& relPath)
{
    const int slash = relPath.lastIndexOf('/');
    return slash < 0 ? QString() : relPath.left(slash);
}

QString DriveService::folderIdOf(const QString& relPath) const
{
    return relPath.isEmpty() ? folderId() : m_fileIds.value(relPath);
}

void DriveService::showError(const std::string& title, const QString& text)
{
    interactive()->error(title, IInteractive::Text(text.toStdString()));
}

bool DriveService::checkCanChange(const QString& relPath)
{
    if (m_engine) {
        showError(muse::trc("drive", "Syncing"), muse::qtrc("drive", "Try again in a moment."));
        return false;
    }

    const auto item = m_items.constFind(relPath);
    if (item == m_items.cend()) {
        return false;
    }

    // Not while open, here or in another window (a folder: none of its scores)
    if (item->isFolder) {
        for (const Item& other : std::as_const(m_items)) {
            if (!other.isFolder && other.path.startsWith(relPath + '/') && isOpenInApp(other.path)) {
                showError(muse::trc("drive", "A score of this folder is open"), muse::qtrc("drive", "Close it first."));
                return false;
            }
        }
    } else if (isOpenInApp(relPath)) {
        showError(muse::trc("drive", "This score is open"), muse::qtrc("drive", "Close it first."));
        return false;
    }

    if (!item->lockedBy.isEmpty() && !item->lockedHere && !item->lockStale) {
        showError(muse::trc("drive", "This score is being edited"),
                  muse::qtrc("drive", "%1 has unsaved changes on it.").arg(item->lockedBy));
        return false;
    }
    return true;
}

void DriveService::createFolder(const QString& parentRelPath, const QString& name)
{
    const QString clean = name.trimmed();
    const QString relPath = parentRelPath.isEmpty() ? clean : parentRelPath + '/' + clean;
    if (!isValidName(clean)) {
        showError(muse::trc("drive", "The folder was not created"), muse::qtrc("drive", "This name cannot be used."));
        return;
    }
    if (m_items.contains(relPath)) {
        showError(muse::trc("drive", "The folder was not created"), muse::qtrc("drive", "There is one with this name already."));
        return;
    }
    const QString parentId = folderIdOf(parentRelPath);
    if (parentId.isEmpty()) {
        showError(muse::trc("drive", "The folder was not created"), muse::qtrc("drive", "Sync first, then try again."));
        return;
    }

    m_client->createFolder(parentId, clean, [this, relPath](drivesync::Result<drivesync::DriveFile> result) {
        if (!result.ok()) {
            showError(muse::trc("drive", "The folder was not created"), result.error.toString());
            return;
        }
        Item folder;
        folder.path = relPath;
        folder.isFolder = true;
        m_items.insert(relPath, folder);
        m_fileIds.insert(relPath, result.value->id);
        QDir().mkpath(absPath(relPath));
        saveCatalog();
        m_itemsChanged.notify();
    });
}

void DriveService::rename(const QString& relPath, const QString& newName)
{
    const auto item = m_items.constFind(relPath);
    if (item == m_items.cend()) {
        return;
    }

    QString name = newName.trimmed();
    if (!item->isFolder && !name.endsWith(QStringLiteral(".mscz"), Qt::CaseInsensitive)) {
        name += QStringLiteral(".mscz");
    }
    const QString parent = parentOf(relPath);
    const QString newRelPath = parent.isEmpty() ? name : parent + '/' + name;
    if (newRelPath == relPath) {
        return;
    }
    if (!isValidName(name) || name.compare(QStringLiteral(".mscz"), Qt::CaseInsensitive) == 0) {
        showError(muse::trc("drive", "Not renamed"), muse::qtrc("drive", "This name cannot be used."));
        return;
    }
    if (m_items.contains(newRelPath)) {
        showError(muse::trc("drive", "Not renamed"), muse::qtrc("drive", "There is one with this name already."));
        return;
    }
    if (!checkCanChange(relPath)) {
        return;
    }

    relocate(relPath, newRelPath, name, QString(), QString());
}

void DriveService::move(const QString& relPath, const QString& targetFolderRelPath)
{
    const auto item = m_items.constFind(relPath);
    if (item == m_items.cend() || parentOf(relPath) == targetFolderRelPath) {
        return;
    }
    if (item->isFolder && (targetFolderRelPath == relPath || targetFolderRelPath.startsWith(relPath + '/'))) {
        showError(muse::trc("drive", "Not moved"), muse::qtrc("drive", "A folder cannot go inside itself."));
        return;
    }

    const QString name = relPath.mid(relPath.lastIndexOf('/') + 1);
    const QString newRelPath = targetFolderRelPath.isEmpty() ? name : targetFolderRelPath + '/' + name;
    if (m_items.contains(newRelPath)) {
        showError(muse::trc("drive", "Not moved"), muse::qtrc("drive", "That folder has one with this name already."));
        return;
    }
    if (!checkCanChange(relPath)) {
        return;
    }

    const QString fromId = folderIdOf(parentOf(relPath));
    const QString toId = folderIdOf(targetFolderRelPath);
    if (toId.isEmpty() || fromId.isEmpty()) {
        showError(muse::trc("drive", "Not moved"), muse::qtrc("drive", "Sync first, then try again."));
        return;
    }
    relocate(relPath, newRelPath, QString(), fromId, toId);
}

void DriveService::relocate(const QString& relPath, const QString& newRelPath, const QString& newName,
                            const QString& fromParentId, const QString& toParentId)
{
    const QString fileId = fileIdOf(relPath);

    auto followLocally = [this, relPath, newRelPath]() {
        // Not while another window syncs the folder
        if (!m_syncLock) {
            m_syncLock = std::make_unique<HeartbeatFile>(rootPath().toQString() + "/.drivesync/sync.lock");
        }
        const bool locked = m_syncLock->acquire();
        QString error;
        const bool ok = drivesync::SyncEngine::relocateLocal(rootPath().toQString(), relPath, newRelPath, &error);
        if (locked) {
            m_syncLock->release();
        }
        if (!ok) {
            LOGW() << "Drive: local copy not moved: " << error.toStdString();
        }
        moveItems(relPath, newRelPath);
        scheduleSync(0);
    };

    if (fileId.isEmpty()) {
        // Only here so far (not uploaded yet): moving the file is enough
        followLocally();
        return;
    }

    m_client->moveRename(fileId, newName, fromParentId, toParentId,
                         [this, followLocally](drivesync::Result<drivesync::DriveFile> result) {
        if (!result.ok()) {
            showError(muse::trc("drive", "Not changed on Drive"), result.error.toString());
            return;
        }
        followLocally();
    });
}

void DriveService::moveItems(const QString& fromRelPath, const QString& toRelPath)
{
    QHash<QString, Item> items;
    QHash<QString, QString> ids;
    for (auto it = m_items.cbegin(); it != m_items.cend(); ++it) {
        QString path = it.key();
        if (path == fromRelPath || path.startsWith(fromRelPath + '/')) {
            path = toRelPath + path.mid(fromRelPath.size());
        }
        Item item = it.value();
        item.path = path;
        items.insert(path, item);
        if (m_fileIds.contains(it.key())) {
            ids.insert(path, m_fileIds.value(it.key()));
        }
    }
    m_items = items;
    m_fileIds = ids;
    saveCatalog();
    m_itemsChanged.notify();
}
