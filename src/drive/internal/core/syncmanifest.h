// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QHash>
#include <QString>

namespace drivesync {
//! What both sides looked like after the last successful transfer of a file.
//! "base" is the common ancestor: comparing each side to it tells who changed what.
struct ManifestEntry
{
    QString fileId;
    QString baseRevision;   // Drive headRevisionId
    QString baseMd5;        // md5 of the content on both sides

    // Local file stats at that moment, so unchanged files need not be hashed again
    qint64 localSize = -1;
    qint64 localMtimeMs = -1;

    bool operator==(const ManifestEntry& o) const
    {
        return fileId == o.fileId && baseRevision == o.baseRevision && baseMd5 == o.baseMd5
               && localSize == o.localSize && localMtimeMs == o.localMtimeMs;
    }
};

class SyncManifest
{
public:
    QString remoteRootId;
    QHash<QString, ManifestEntry> entries; // key: path relative to the sync root, '/'-separated

    bool load(const QString& filePath);
    bool save(const QString& filePath) const;
};
}
