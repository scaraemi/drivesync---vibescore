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

    struct Status {
        State state = State::NotSignedIn;
        QDateTime lastSync;        // last run that finished (with or without problems)
        bool lastSyncOk = true;
        int lastTransfers = 0;
        QStringList conflicts;     // names of the conflict copies made by the last run
        QStringList problems;      // errors and skipped files of the last run, for the user
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

    //! Copies a score of the library into the Drive folder (then syncs). Returns the copy.
    virtual muse::RetVal<muse::io::path_t> copyToDrive(const muse::io::path_t& scorePath) = 0;
};
}
