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
#pragma once

#include <memory>

#include <QHash>
#include <QUrl>
#include <QObject>
#include <QSet>
#include <QTimer>

#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "actions/iactionsdispatcher.h"
#include "context/iglobalcontext.h"
#include "global/iinteractive.h"
#include "global/iglobalconfiguration.h"
#include "global/io/ifilesystem.h"
#include "project/iprojectconfiguration.h"

#include "../idriveservice.h"
#include "heartbeatfile.h"

class QNetworkAccessManager;

namespace drivesync {
class GoogleAuth;
class DriveClient;
class SyncEngine;
class SettingsTokenStore;
struct SyncReport;
struct DriveFile;
}

namespace mu::drive {
class DriveService : public QObject, public IDriveService, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    muse::GlobalInject<muse::io::IFileSystem> fileSystem;
    muse::GlobalInject<project::IProjectConfiguration> projectConfiguration;
    muse::GlobalInject<muse::IGlobalConfiguration> globalConfiguration;
    muse::ContextInject<context::IGlobalContext> globalContext = { this };
    muse::ContextInject<muse::IInteractive> interactive = { this };
    muse::ContextInject<muse::actions::IActionsDispatcher> dispatcher = { this };

public:
    explicit DriveService(const muse::modularity::ContextPtr& iocCtx);
    ~DriveService() override;

    void init();
    void deinit();

    bool isAvailable() const override;
    Status status() const override;
    muse::async::Notification statusChanged() const override;

    muse::io::path_t rootPath() const override;

    void signIn() override;
    void cancelSignIn() override;
    void signOut() override;

    QString folderId() const override;
    muse::Ret setFolder(const QString& linkOrId) override;

    void sync() override;

    QList<Item> items() const override;
    muse::async::Notification itemsChanged() const override;
    void openScore(const QString& relPath) override;
    muse::Ret removeFromDevice(const QString& relPath) override;
    void deleteFromDrive(const QString& relPath) override;
    void unlock(const QString& relPath) override;
    void createFolder(const QString& parentRelPath, const QString& name) override;
    void rename(const QString& relPath, const QString& newName) override;
    void move(const QString& relPath, const QString& targetFolderRelPath) override;
    void shareDiagnostics() override;
    QString accountName() const override;
    QString accountEmail() const override;
    muse::async::Notification accountChanged() const override;
    void checkForUpdates(bool userAsked) override;

    muse::RetVal<muse::io::path_t> copyToDrive(const muse::io::path_t& scorePath) override;

    static QString folderIdFromLink(const QString& linkOrId);

private:
    struct OpenScore {
        QString relPath;           // empty: the open score is not in the Drive folder
        QString fileId;            // empty: not on Drive yet
        QString knownRevision;     // the revision the app has (or was told about)
        bool lockHeld = false;     // this device locked it (unsaved changes)
        bool warnedLocked = false; // told the user that someone else is editing it
    };

    void updateState();
    void scheduleSync(int delayMs);
    void onFinished(const drivesync::SyncReport& report);
    bool isOpenInApp(const QString& relPath) const;
    QString relPathOf(const muse::io::path_t& path) const;
    QString absPath(const QString& relPath) const;

    // The open score
    void onCurrentProjectChanged();
    void markOpenProject();
    QString openMarkerPath(const QString& relPath) const;
    void checkOpenScore(bool opening);
    void onOpenScoreModified();
    void takeLock();
    void releaseLock(const QString& fileId);
    void reloadOpenScore();
    void onPollTimer();

    // Locks: the reason carries the device and the expiry, e.g. "Unsaved changes on Tablet [vs:<id>:<until>]"
    QString lockReasonText() const;
    bool isOwnLock(const drivesync::DriveFile& file) const;
    bool isStaleLock(const drivesync::DriveFile& file) const;
    QString deviceId() const;

    // The catalog of the Drive folder
    void updateCatalog(const drivesync::SyncEngine& engine);
    void refreshLocalState();
    void loadCatalog();
    void saveCatalog() const;
    QString fileIdOf(const QString& relPath) const;
    //! The analysis layers of a score (relative paths, on Drive or here)
    QStringList companionsOf(const QString& scoreRelPath) const;
    bool isScoreOnDevice(const QString& companionRelPath) const;
    void loadAccount();
    QString folderIdOf(const QString& relPath) const;

    // Rename / move
    bool checkCanChange(const QString& relPath);
    void relocate(const QString& relPath, const QString& newRelPath, const QString& newName, const QString& fromParentId,
                  const QString& toParentId);
    void relocateOne(const QString& relPath, const QString& newRelPath, const QString& newName, const QString& fromParentId,
                     const QString& toParentId);
    void moveItems(const QString& fromRelPath, const QString& toRelPath);
    void showError(const std::string& title, const QString& text);

    // Diagnostics and updates (driveservice_extras.cpp)
    void initExtras();
    QString logsPath() const;
    void offerCrashReport();
    void downloadAndInstall(const QString& tag, const QUrl& url, qint64 expectedSize);
    void installDownloadedUpdate(const QString& apkPath);
    void onAppActiveForUpdate();

    QNetworkAccessManager* m_nam = nullptr;
    std::unique_ptr<drivesync::SettingsTokenStore> m_tokenStore;
    drivesync::GoogleAuth* m_auth = nullptr;
    drivesync::DriveClient* m_client = nullptr;
    drivesync::SyncEngine* m_engine = nullptr;

    std::unique_ptr<HeartbeatFile> m_syncLock;
    std::unique_ptr<HeartbeatFile> m_openMarker;

    QTimer m_syncTimer;
    QTimer m_pollTimer;
    QTimer m_lockRenewTimer;
    int m_pollTicks = 0;
    bool m_syncAgain = false;
    QDateTime m_lastStart;
    Status m_status;
    muse::async::Notification m_statusChanged;

    QHash<QString, Item> m_items;          // path -> item
    QHash<QString, QString> m_fileIds;     // path -> Drive file id
    muse::async::Notification m_itemsChanged;

    QSet<QString> m_wanted;                // to download on the next run (on demand)
    QString m_openAfterSync;               // relative path
    QString m_reopenAfterClose;            // relative path

    OpenScore m_open;

    QTimer m_updateTimer;
    QString m_updateDismissed;             // "Later" for this release, while the app runs
    bool m_updatePromptOpen = false;
    bool m_updateDownloading = false;
    QString m_pendingUpdateApk;            // downloaded, waiting for the permission to install

    QString m_accountName;
    QString m_accountEmail;
    muse::async::Notification m_accountChanged;
};
}
