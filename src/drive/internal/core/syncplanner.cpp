// SPDX-License-Identifier: GPL-3.0-only
#include "syncplanner.h"

#include <QSet>

#include <algorithm>

using namespace drivesync;

const char* drivesync::toString(SyncAction action)
{
    switch (action) {
    case SyncAction::None: return "None";
    case SyncAction::Download: return "Download";
    case SyncAction::UploadNew: return "UploadNew";
    case SyncAction::UploadUpdate: return "UploadUpdate";
    case SyncAction::DeleteLocal: return "DeleteLocal";
    case SyncAction::DeleteRemote: return "DeleteRemote";
    case SyncAction::Adopt: return "Adopt";
    case SyncAction::Forget: return "Forget";
    case SyncAction::Conflict: return "Conflict";
    }
    return "?";
}

SyncAction SyncPlanner::decide(const LocalState* local, const RemoteState* remote, const ManifestEntry* base)
{
    if (!base) {
        if (local && remote) {
            return local->md5 == remote->md5 ? SyncAction::Adopt : SyncAction::Conflict;
        }
        if (local) {
            return SyncAction::UploadNew;
        }
        if (remote) {
            return SyncAction::Download;
        }
        return SyncAction::None;
    }

    // A different file id at the same path means the remote file was replaced: its revision is not comparable.
    const bool remoteReplaced = remote && remote->fileId != base->fileId;
    const bool localChanged = local && local->md5 != base->baseMd5;
    const bool remoteChanged = remote && (remoteReplaced || remote->revision != base->baseRevision);

    if (local && remote) {
        if (!localChanged && !remoteChanged) {
            return SyncAction::None;
        }
        if (localChanged && remoteChanged) {
            return local->md5 == remote->md5 ? SyncAction::Adopt : SyncAction::Conflict;
        }
        if (localChanged) {
            return SyncAction::UploadUpdate;
        }
        return SyncAction::Download;
    }

    if (local) { // deleted remotely
        // Never lose local work: if it changed, put it back on Drive as a new file
        return localChanged ? SyncAction::UploadNew : SyncAction::DeleteLocal;
    }

    if (remote) { // deleted locally
        // Never lose remote work: if somebody changed it in the meantime, bring it back
        return remoteChanged ? SyncAction::Download : SyncAction::DeleteRemote;
    }

    return SyncAction::Forget;
}

QList<PlannedItem> SyncPlanner::plan(const QHash<QString, LocalState>& local, const QHash<QString, RemoteState>& remote,
                                     const QHash<QString, ManifestEntry>& base)
{
    QSet<QString> paths;
    for (auto it = local.keyBegin(); it != local.keyEnd(); ++it) {
        paths.insert(*it);
    }
    for (auto it = remote.keyBegin(); it != remote.keyEnd(); ++it) {
        paths.insert(*it);
    }
    for (auto it = base.keyBegin(); it != base.keyEnd(); ++it) {
        paths.insert(*it);
    }

    QStringList sorted(paths.begin(), paths.end());
    std::sort(sorted.begin(), sorted.end());

    QList<PlannedItem> items;
    for (const QString& path : sorted) {
        const auto l = local.find(path);
        const auto r = remote.find(path);
        const auto b = base.find(path);

        const SyncAction action = decide(l != local.end() ? &l.value() : nullptr,
                                         r != remote.end() ? &r.value() : nullptr,
                                         b != base.end() ? &b.value() : nullptr);
        if (action != SyncAction::None) {
            items.append({ path, action });
        }
    }
    return items;
}
