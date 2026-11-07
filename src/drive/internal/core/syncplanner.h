// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include "syncmanifest.h"

namespace drivesync {
enum class SyncAction {
    None,
    Download,       // remote is new or changed, local is not
    UploadNew,      // local is new (or the remote copy was deleted while the local one changed)
    UploadUpdate,   // local changed, remote did not
    DeleteLocal,    // remote deleted, local unchanged
    DeleteRemote,   // local deleted, remote unchanged
    Adopt,          // both sides already have the same content: just record it
    Forget,         // gone on both sides
    Conflict,       // both sides changed differently
};

const char* toString(SyncAction action);

struct LocalState
{
    QString md5;
};

struct RemoteState
{
    QString fileId;
    QString revision;
    QString md5;
};

struct PlannedItem
{
    QString path;
    SyncAction action = SyncAction::None;
};

//! Three-way comparison: local and remote against the last synced state.
//! Pure functions, no I/O.
class SyncPlanner
{
public:
    static SyncAction decide(const LocalState* local, const RemoteState* remote, const ManifestEntry* base);

    //! One item per path known to any of the three sides, sorted by path. Items with action None are left out.
    static QList<PlannedItem> plan(const QHash<QString, LocalState>& local, const QHash<QString, RemoteState>& remote,
                                   const QHash<QString, ManifestEntry>& base);
};
}
