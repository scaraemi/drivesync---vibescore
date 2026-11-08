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

#include <QObject>
#include <QTimer>

#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "context/iglobalcontext.h"
#include "global/iinteractive.h"
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
}

namespace mu::drive {
class DriveService : public QObject, public IDriveService, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    muse::GlobalInject<muse::io::IFileSystem> fileSystem;
    muse::GlobalInject<project::IProjectConfiguration> projectConfiguration;
    muse::ContextInject<context::IGlobalContext> globalContext = { this };
    muse::ContextInject<muse::IInteractive> interactive = { this };

public:
    explicit DriveService(const muse::modularity::ContextPtr& iocCtx);
    ~DriveService() override;

    void init();

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

    muse::RetVal<muse::io::path_t> copyToDrive(const muse::io::path_t& scorePath) override;

    static QString folderIdFromLink(const QString& linkOrId);

private:
    void updateState();
    void scheduleSync(int delayMs);
    void onFinished(const drivesync::SyncReport& report);
    void watchCurrentProject();
    void markOpenProject();
    bool isOpenInApp(const QString& relPath) const;
    QString openMarkerPath(const QString& relPath) const;

    QNetworkAccessManager* m_nam = nullptr;
    std::unique_ptr<drivesync::SettingsTokenStore> m_tokenStore;
    drivesync::GoogleAuth* m_auth = nullptr;
    drivesync::DriveClient* m_client = nullptr;
    drivesync::SyncEngine* m_engine = nullptr;

    std::unique_ptr<HeartbeatFile> m_syncLock;
    std::unique_ptr<HeartbeatFile> m_openMarker;

    QTimer m_syncTimer;
    bool m_syncAgain = false;
    QDateTime m_lastStart;
    Status m_status;
    muse::async::Notification m_statusChanged;
};
}
