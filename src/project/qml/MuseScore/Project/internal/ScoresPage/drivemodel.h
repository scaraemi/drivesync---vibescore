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

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <qqmlintegration.h>

#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "global/iinteractive.h"

#include "drive/idriveservice.h"

namespace mu::project {
//! The Drive tab of the Scores page: Google sign-in, the shared folder, the state of the sync, and the scores of
//! the folder (most only listed: they are downloaded when opened).
//! Empty (isAvailable false) when the app has no drive module or no Google OAuth client.
class DriveModel : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    Q_PROPERTY(bool isAvailable READ isAvailable CONSTANT)
    Q_PROPERTY(QString state READ state NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QStringList problems READ problems NOTIFY statusChanged)
    Q_PROPERTY(QString rootPath READ rootPath CONSTANT)

    //! The folder shown ("" = the Drive folder itself), and its scores and subfolders
    Q_PROPERTY(QString currentFolder READ currentFolder NOTIFY itemsChanged)
    Q_PROPERTY(QString currentFolderName READ currentFolderName NOTIFY itemsChanged)
    //! With searchText: the matching scores of every folder
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY itemsChanged)
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)

    QML_ELEMENT

    muse::ContextInject<drive::IDriveService> driveService = { this };
    muse::ContextInject<muse::IInteractive> interactive = { this };

public:
    explicit DriveModel(QObject* parent = nullptr);

    bool isAvailable() const;
    //! "notSignedIn", "signingIn", "noFolder", "idle", "syncing"
    QString state() const;
    QString statusText() const;
    QStringList problems() const;
    QString rootPath() const;

    Q_INVOKABLE void load();
    Q_INVOKABLE void signIn();
    Q_INVOKABLE void cancelSignIn();
    Q_INVOKABLE void signOut();
    //! Returns an error message, or an empty string when it worked
    Q_INVOKABLE QString setFolder(const QString& linkOrId);
    Q_INVOKABLE void syncNow();
    Q_INVOKABLE void copyToDrive(const QString& scorePath);

    QString currentFolder() const;
    QString currentFolderName() const;
    QString searchText() const;
    void setSearchText(const QString& text);
    QVariantList items() const;

    Q_INVOKABLE void openFolder(const QString& path);
    Q_INVOKABLE void goUp();
    //! A score: opened (downloaded first if needed); a folder: shown
    Q_INVOKABLE void openItem(const QString& path);
    Q_INVOKABLE void removeFromDevice(const QString& path);
    Q_INVOKABLE void deleteFromDrive(const QString& path);
    Q_INVOKABLE void unlock(const QString& path);

signals:
    void statusChanged();
    //! A sync run finished: the scores of the folder may have changed
    void syncFinished();
    void itemsChanged();

private:
    QVariantMap toMap(const drive::IDriveService::Item& item) const;

    QString m_lastState;
    QString m_currentFolder;
    QString m_searchText;
};
}
