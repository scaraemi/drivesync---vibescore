// SPDX-License-Identifier: GPL-3.0-only
#include "syncengine.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSysInfo>

using namespace drivesync;

static QString parentDir(const QString& relPath)
{
    const int slash = relPath.lastIndexOf('/');
    return slash < 0 ? QString() : relPath.left(slash);
}

static QString fileName(const QString& relPath)
{
    return relPath.mid(relPath.lastIndexOf('/') + 1);
}

static QString md5Of(const QString& path, bool* ok)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *ok = false;
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Md5);
    *ok = hash.addData(&file);
    return QString::fromLatin1(hash.result().toHex());
}

SyncEngine::SyncEngine(DriveClient* client, SyncConfig config, QObject* parent)
    : QObject(parent), m_client(client), m_config(std::move(config))
{
    m_config.localRoot = QDir::cleanPath(m_config.localRoot);
    if (m_config.deviceLabel.isEmpty()) {
        m_config.deviceLabel = QSysInfo::machineHostName();
    }
}

QString SyncEngine::manifestPath() const
{
    return m_config.localRoot + "/.drivesync/manifest.json";
}

QString SyncEngine::localTrashPath() const
{
    return m_config.localRoot + "/.drivesync/trash";
}

QString SyncEngine::abs(const QString& relPath) const
{
    return m_config.localRoot + '/' + relPath;
}

QString SyncEngine::conflictCopyName(const QString& path, const QString& label, const QString& device, const QDateTime& when)
{
    const QString dir = parentDir(path);
    const QString name = fileName(path);
    const int dot = name.lastIndexOf('.');
    const QString base = dot > 0 ? name.left(dot) : name;
    const QString ext = dot > 0 ? name.mid(dot) : QString();

    QStringList parts { label };
    if (!device.isEmpty()) {
        parts << device;
    }
    parts << when.toString("yyyy-MM-dd HHmm");

    const QString copy = QStringLiteral("%1 (%2)%3").arg(base, parts.join(' '), ext);
    return dir.isEmpty() ? copy : dir + '/' + copy;
}

QString SyncEngine::uniqueConflictPath(const QString& path) const
{
    const QString first = conflictCopyName(path, m_config.conflictLabel, m_config.deviceLabel, m_clock());
    QString candidate = first;
    for (int i = 2; QFileInfo::exists(abs(candidate)) || m_remote.contains(candidate)
         || m_manifest.entries.contains(candidate); ++i) {
        const int dot = first.lastIndexOf('.');
        candidate = dot > first.lastIndexOf('/') + 1
                    ? QStringLiteral("%1 %2%3").arg(first.left(dot)).arg(i).arg(first.mid(dot))
                    : QStringLiteral("%1 %2").arg(first).arg(i);
    }
    return candidate;
}

bool SyncEngine::isExcluded(const QString& relPath) const
{
    const QStringList segments = relPath.split('/');
    for (const QString& s : segments) {
        if (s.startsWith('.')) {
            return true;
        }
    }

    const QString name = segments.last();
    for (const QString& pattern : m_config.excludePatterns) {
        const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern),
                                    QRegularExpression::CaseInsensitiveOption);
        if (re.match(name).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool SyncEngine::isSyncedFile(const QString& relPath) const
{
    if (isExcluded(relPath)) {
        return false;
    }
    if (m_config.includePatterns.isEmpty()) {
        return true;
    }

    const QString name = relPath.mid(relPath.lastIndexOf('/') + 1);
    for (const QString& pattern : m_config.includePatterns) {
        const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern),
                                    QRegularExpression::CaseInsensitiveOption);
        if (re.match(name).hasMatch()) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Run

void SyncEngine::run()
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_report = SyncReport();
    m_remote.clear();
    m_local.clear();
    m_queue.clear();
    m_duplicateRemote.clear();

    m_manifest.load(manifestPath());
    if (m_manifest.remoteRootId != m_config.remoteRootId) {
        // Another Drive folder: nothing from the old one is a valid base
        m_manifest.entries.clear();
        m_manifest.remoteRootId = m_config.remoteRootId;
    }

    listRemote();
}

void SyncEngine::listRemote()
{
    m_remoteDirs.clear();
    m_remoteDirs.insert(QString(), m_config.remoteRootId);
    m_folderQueue = { { m_config.remoteRootId, QString() } };
    listNextRemoteFolder();
}

void SyncEngine::listNextRemoteFolder()
{
    if (m_folderQueue.isEmpty()) {
        planAndProcess();
        return;
    }

    const auto [folderId, relDir] = m_folderQueue.takeFirst();

    m_client->listChildren(folderId, [this, relDir = relDir](Result<QList<DriveFile> > result) {
        if (!result.ok()) {
            m_report.errors << QStringLiteral("Cannot list the Drive folder '%1': %2").arg(relDir, result.error.toString());
            m_report.aborted = true;
            finish();
            return;
        }

        for (const DriveFile& f : *result.value) {
            if (f.name.contains('/')) {
                m_report.skipped << QStringLiteral("%1/%2: '/' in a Drive name").arg(relDir, f.name);
                continue;
            }
            const QString rel = relDir.isEmpty() ? f.name : relDir + '/' + f.name;
            if (f.isGoogleNative() || (f.isFolder() ? isExcluded(rel) : !isSyncedFile(rel))) {
                continue;
            }

            if (f.isFolder()) {
                if (m_remoteDirs.contains(rel)) {
                    m_report.skipped << rel + ": more than one Drive folder with this name";
                    continue;
                }
                m_remoteDirs.insert(rel, f.id);
                m_folderQueue.append({ f.id, rel });
            } else if (m_remote.contains(rel) || m_duplicateRemote.contains(rel)) {
                // Drive allows duplicate names; a file system does not. Leave the path alone until it is fixed.
                m_remote.remove(rel);
                if (!m_duplicateRemote.contains(rel)) {
                    m_duplicateRemote.insert(rel);
                    m_report.skipped << rel + ": more than one Drive file with this name";
                }
            } else {
                m_remote.insert(rel, f);
            }
        }

        listNextRemoteFolder();
    });
}

bool SyncEngine::scanLocal()
{
    QDir().mkpath(m_config.localRoot);
    const QDir root(m_config.localRoot);

    QDirIterator it(m_config.localRoot, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString rel = root.relativeFilePath(path);
        if (!isSyncedFile(rel)) {
            continue;
        }

        const QFileInfo info(path);
        LocalFile local;
        local.size = info.size();
        local.mtimeMs = info.lastModified().toMSecsSinceEpoch();

        const auto base = m_manifest.entries.constFind(rel);
        if (base != m_manifest.entries.cend() && base->localSize == local.size && base->localMtimeMs == local.mtimeMs) {
            local.md5 = base->baseMd5; // unchanged since the last sync
        } else {
            bool ok = false;
            local.md5 = md5Of(path, &ok);
            if (!ok) {
                m_report.skipped << rel + ": cannot read the file";
                m_duplicateRemote.insert(rel); // keep the path out of the plan, so it is not deleted remotely
                continue;
            }
        }
        m_local.insert(rel, local);
    }
    return true;
}

void SyncEngine::planAndProcess()
{
    scanLocal();

    QHash<QString, LocalState> local;
    for (auto it = m_local.cbegin(); it != m_local.cend(); ++it) {
        local.insert(it.key(), LocalState { it->md5 });
    }

    QHash<QString, RemoteState> remote;
    for (auto it = m_remote.cbegin(); it != m_remote.cend(); ++it) {
        remote.insert(it.key(), RemoteState { it->id, it->headRevisionId, it->md5 });
    }

    QHash<QString, ManifestEntry> base = m_manifest.entries;

    // Paths that cannot be compared safely stay out of this run
    for (const QString& path : std::as_const(m_duplicateRemote)) {
        local.remove(path);
        remote.remove(path);
        base.remove(path);
    }

    m_queue = SyncPlanner::plan(local, remote, base);

    int deletions = 0;
    for (const PlannedItem& item : std::as_const(m_queue)) {
        if (item.action == SyncAction::DeleteLocal || item.action == SyncAction::DeleteRemote) {
            ++deletions;
        }
    }
    const int synced = m_manifest.entries.size();
    if (!m_config.allowMassDelete && deletions >= m_config.minDeletionsForGuard
        && deletions > m_config.maxDeleteShare * synced) {
        m_report.errors << QStringLiteral("Stopped: %1 of %2 synced files would be deleted. "
                                          "Check the folders, then allow it explicitly.").arg(deletions).arg(synced);
        m_report.aborted = true;
        m_queue.clear();
        finish();
        return;
    }

    processNext();
}

void SyncEngine::processNext()
{
    if (m_queue.isEmpty()) {
        finish();
        return;
    }

    const PlannedItem item = m_queue.takeFirst();
    emit itemStarted(item.path, item.action);
    process(item);
}

void SyncEngine::done(const QString& path, const QString& error)
{
    if (!error.isEmpty()) {
        m_report.errors << path + ": " + error;
    }
    // Queued, so long runs do not grow the stack
    QMetaObject::invokeMethod(this, &SyncEngine::processNext, Qt::QueuedConnection);
}

void SyncEngine::finish()
{
    if (!m_manifest.save(manifestPath())) {
        m_report.errors << "Cannot save " + manifestPath();
    }
    m_running = false;
    emit finished(m_report);
}

void SyncEngine::process(const PlannedItem& item)
{
    const QString& path = item.path;

    const bool changesLocalFile = item.action == SyncAction::Download || item.action == SyncAction::DeleteLocal
                                  || item.action == SyncAction::Conflict;
    if (changesLocalFile && m_config.isOpenInApp && m_config.isOpenInApp(path)) {
        m_report.skipped << path + ": open in the app";
        done(path);
        return;
    }

    switch (item.action) {
    case SyncAction::None:
        done(path);
        return;
    case SyncAction::Adopt:
        record(path, m_remote.value(path));
        done(path);
        return;
    case SyncAction::Forget:
        m_manifest.entries.remove(path);
        done(path);
        return;
    case SyncAction::Download:
        if (!localUnchangedSinceScan(path)) {
            m_report.skipped << path + ": changed locally during the sync";
            done(path);
            return;
        }
        download(path, m_remote.value(path), [this, path]() { done(path); });
        return;
    case SyncAction::UploadNew:
        uploadNew(path, [this, path]() { done(path); });
        return;
    case SyncAction::UploadUpdate:
        uploadUpdate(path);
        return;
    case SyncAction::DeleteLocal:
        deleteLocal(path);
        return;
    case SyncAction::DeleteRemote:
        deleteRemote(path);
        return;
    case SyncAction::Conflict:
        resolveConflict(path);
        return;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Actions. Each one ends with done(): directly on failure, through `then` on success.

bool SyncEngine::localUnchangedSinceScan(const QString& path) const
{
    const QFileInfo info(abs(path));
    const auto scanned = m_local.constFind(path);
    if (scanned == m_local.cend()) {
        return !info.exists();
    }
    return info.exists() && info.size() == scanned->size && info.lastModified().toMSecsSinceEpoch() == scanned->mtimeMs;
}

void SyncEngine::record(const QString& path, const DriveFile& remote)
{
    // Stats from the scan when the content was read then (uploads, adopt): if the file changed after the scan,
    // the stats will not match and the next run hashes it again. Stats from disk when we just wrote it.
    ManifestEntry entry { remote.id, remote.headRevisionId, remote.md5, -1, -1 };
    const auto scanned = m_local.constFind(path);
    if (scanned != m_local.cend()) {
        entry.localSize = scanned->size;
        entry.localMtimeMs = scanned->mtimeMs;
    } else {
        const QFileInfo info(abs(path));
        entry.localSize = info.size();
        entry.localMtimeMs = info.lastModified().toMSecsSinceEpoch();
    }
    m_manifest.entries.insert(path, entry);
}

void SyncEngine::download(const QString& path, const DriveFile& remote, std::function<void()> then)
{
    m_client->download(remote.id, abs(path), [=](Result<QString> result) {
        if (!result.ok()) {
            done(path, "download failed: " + result.error.toString());
            return;
        }
        if (!remote.md5.isEmpty() && *result.value != remote.md5) {
            done(path, "download corrupted (checksum mismatch)");
            return;
        }
        m_local.remove(path); // the file on disk is new: take its stats from disk
        record(path, remote);
        ++m_report.downloaded;
        then();
    });
}

void SyncEngine::uploadNew(const QString& path, std::function<void()> then)
{
    ensureRemoteDir(parentDir(path), [=](const QString& folderId, const DriveError& error) {
        if (folderId.isEmpty()) {
            done(path, "cannot create the Drive folder: " + error.toString());
            return;
        }
        m_client->createFile(folderId, fileName(path), abs(path), [=](Result<DriveFile> result) {
            if (!result.ok()) {
                done(path, "upload failed: " + result.error.toString());
                return;
            }
            m_remote.insert(path, *result.value);
            record(path, *result.value);
            ++m_report.uploaded;
            then();
        });
    });
}

void SyncEngine::uploadUpdate(const QString& path)
{
    const ManifestEntry base = m_manifest.entries.value(path);

    // Optimistic check: the plan was made from a listing that may be minutes old
    m_client->getFile(base.fileId, [=](Result<DriveFile> current) {
        if (!current.ok() && current.error.httpStatus == 404) {
            m_manifest.entries.remove(path);
            uploadNew(path, [this, path]() { done(path); });
            return;
        }
        if (!current.ok()) {
            done(path, current.error.toString());
            return;
        }

        const DriveFile remote = *current.value;
        if (remote.trashed) {
            m_manifest.entries.remove(path);
            uploadNew(path, [this, path]() { done(path); });
            return;
        }
        if (remote.headRevisionId != base.baseRevision) {
            m_remote.insert(path, remote);
            resolveConflict(path);
            return;
        }
        if (remote.locked) {
            m_report.skipped << QStringLiteral("%1: locked on Drive%2").arg(path,
                                                                           remote.lockReason.isEmpty() ? QString()
                                                                           : " (" + remote.lockReason + ")");
            done(path);
            return;
        }

        m_client->updateContent(base.fileId, abs(path), [=](Result<DriveFile> result) {
            if (!result.ok()) {
                done(path, "upload failed: " + result.error.toString());
                return;
            }
            m_remote.insert(path, *result.value);
            record(path, *result.value);
            ++m_report.uploaded;
            done(path);
        });
    });
}

void SyncEngine::deleteLocal(const QString& path)
{
    if (!localUnchangedSinceScan(path)) {
        m_report.skipped << path + ": changed locally during the sync";
        done(path);
        return;
    }

    const QString target = localTrashPath() + '/' + m_clock().toString("yyyyMMdd-HHmmss") + '/' + path;
    QDir().mkpath(QFileInfo(target).absolutePath());
    if (!QFile::rename(abs(path), target)) {
        done(path, "cannot move the local file to " + target);
        return;
    }

    m_manifest.entries.remove(path);
    ++m_report.deletedLocal;
    done(path);
}

void SyncEngine::deleteRemote(const QString& path)
{
    const ManifestEntry base = m_manifest.entries.value(path);

    m_client->getFile(base.fileId, [=](Result<DriveFile> current) {
        if (!current.ok() && current.error.httpStatus == 404) {
            m_manifest.entries.remove(path);
            done(path);
            return;
        }
        if (!current.ok()) {
            done(path, current.error.toString());
            return;
        }
        if (current.value->trashed) {
            m_manifest.entries.remove(path);
            done(path);
            return;
        }
        if (current.value->headRevisionId != base.baseRevision) {
            // Changed on Drive after the listing: bring it back instead of deleting it
            download(path, *current.value, [this, path]() { done(path); });
            return;
        }

        m_client->trash(base.fileId, [=](Result<DriveFile> result) {
            if (!result.ok()) {
                done(path, "cannot move the Drive file to the trash: " + result.error.toString());
                return;
            }
            m_manifest.entries.remove(path);
            m_remote.remove(path);
            ++m_report.deletedRemote;
            done(path);
        });
    });
}

void SyncEngine::resolveConflict(const QString& path)
{
    if (m_config.conflictPolicy == ConflictPolicy::ReportOnly) {
        m_report.conflicts << path;
        done(path);
        return;
    }

    if (!localUnchangedSinceScan(path)) {
        m_report.skipped << path + ": changed locally during the sync";
        done(path);
        return;
    }

    // The remote version keeps the name, the local version becomes a copy next to it, on both sides.
    const QString copy = uniqueConflictPath(path);
    if (!QFile::rename(abs(path), abs(copy))) {
        done(path, "cannot rename the local file to " + copy);
        return;
    }
    m_local.insert(copy, m_local.take(path)); // same content and stats, new name

    download(path, m_remote.value(path), [=]() {
        uploadNew(copy, [=]() {
            m_report.conflicts << copy;
            done(path);
        });
    });
}

void SyncEngine::ensureRemoteDir(const QString& relDir, std::function<void(const QString&, const DriveError&)> then)
{
    const auto known = m_remoteDirs.constFind(relDir);
    if (known != m_remoteDirs.cend()) {
        then(*known, DriveError());
        return;
    }

    ensureRemoteDir(parentDir(relDir), [=](const QString& parentId, const DriveError& error) {
        if (parentId.isEmpty()) {
            then(QString(), error);
            return;
        }
        m_client->createFolder(parentId, fileName(relDir), [=](Result<DriveFile> result) {
            if (!result.ok()) {
                then(QString(), result.error);
                return;
            }
            m_remoteDirs.insert(relDir, result.value->id);
            then(result.value->id, DriveError());
        });
    });
}
