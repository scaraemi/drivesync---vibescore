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

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include "global/modularity/imoduleinterface.h"
#include "global/async/notification.h"
#include "global/io/path.h"
#include "global/types/retval.h"

namespace mu::drive {
//! A shared Google Drive folder of scores (.mscz), kept in sync with a folder of the app (the Drive folder of
//! the Scores library). Scores are opened and saved there like any other; the service syncs at start, when the
//! app comes back to the foreground and after a score in the folder is saved.
//! Header only for the users of the service (the Scores page): they resolve it at run time, so a build without
//! the drive module simply has no Drive tab.
class IDriveService : MODULE_CONTEXT_INTERFACE
{
    INTERFACE_ID(IDriveService)

public:
    virtual ~IDriveService() = default;

    enum class State {
        NotSignedIn,
        SigningIn,   // waiting for the browser
        NoFolder,    // signed in, no Drive folder chosen
        Idle,
        Syncing,
    };

    //! A file or folder of the Drive folder (on demand: most files are only listed until they are opened)
    struct Item {
        QString path;              // relative to the Drive folder, '/'-separated
        bool isFolder = false;
        qint64 size = 0;
        QDateTime modified;
        QString modifiedBy;
        bool onDevice = false;     // downloaded (or not uploaded yet)
        bool notUploaded = false;  // only on this device so far
        QString lockedBy;          // empty: not locked
        QString lockReason;        // e.g. "Unsaved changes on Tablet"
        QDateTime lockedSince;
        bool lockedHere = false;   // by this device
        bool lockStale = false;    // its owner did not renew it: may be lifted
    };

    struct Status {
        State state = State::NotSignedIn;
        QDateTime lastSync;        // last run that finished (with or without problems)
        bool lastSyncOk = true;
        int lastTransfers = 0;
        QStringList conflicts;     // names of the conflict copies made by the last run
        QStringList problems;      // errors and skipped files of the last run, for the user
        QString fetching;          // a score being downloaded to be opened
    };

    //! False when the app was built without a Google OAuth client
    virtual bool isAvailable() const = 0;
    virtual Status status() const = 0;
    virtual muse::async::Notification statusChanged() const = 0;

    //! The local folder of the Drive scores
    virtual muse::io::path_t rootPath() const = 0;

    //! Opens the browser for the Google sign-in
    virtual void signIn() = 0;
    virtual void cancelSignIn() = 0;
    virtual void signOut() = 0;

    virtual QString folderId() const = 0;
    //! A Drive folder link (https://drive.google.com/drive/folders/<id>...) or the bare id
    virtual muse::Ret setFolder(const QString& linkOrId) = 0;

    virtual void sync() = 0;

    //! The whole Drive folder, as of the last sync (kept between runs of the app)
    virtual QList<Item> items() const = 0;
    virtual muse::async::Notification itemsChanged() const = 0;

    //! Opens a score of the folder, downloading it first if it is not on the device
    virtual void openScore(const QString& relPath) = 0;
    //! Frees the space: the score stays on Drive. Refused if it has changes not on Drive yet, or is open.
    virtual muse::Ret removeFromDevice(const QString& relPath) = 0;
    //! Moves the score to Drive's trash (for everyone), and off this device
    virtual void deleteFromDrive(const QString& relPath) = 0;
    //! Lifts the "being edited" lock of a score (e.g. left by a device that is off)
    virtual void unlock(const QString& relPath) = 0;

    //! On Drive (for everyone), and here. Problems are shown to the user.
    virtual void createFolder(const QString& parentRelPath, const QString& name) = 0;
    //! A score keeps its .mscz extension
    virtual void rename(const QString& relPath, const QString& newName) = 0;
    //! Into another folder ("" = the Drive folder itself)
    virtual void move(const QString& relPath, const QString& targetFolderRelPath) = 0;

    //! The app's logs and crash reports: shared (Android) or shown in the file browser (desktop)
    virtual void shareDiagnostics() = 0;
    //! Looks for a newer release of this build on GitHub; userAsked: also says when there is none
    virtual void checkForUpdates(bool userAsked) = 0;

    //! Copies a score of the library into the Drive folder (then syncs). Returns the copy.
    virtual muse::RetVal<muse::io::path_t> copyToDrive(const muse::io::path_t& scorePath) = 0;
};
}
