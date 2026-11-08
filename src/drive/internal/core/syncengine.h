// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>

#include "driveclient.h"
#include "syncmanifest.h"
#include "syncplanner.h"

namespace drivesync {
enum class ConflictPolicy {
    KeepBoth,    // the remote version keeps the name; the local one is renamed and uploaded as a copy
    ReportOnly,  // touch nothing, report the conflict (for a UI that asks the user)
};

struct SyncConfig
{
    QString localRoot;
    QString remoteRootId;          // a folder in My Drive, or a shared drive's id
    QString deviceLabel;           // goes into conflict copy names, e.g. "Emiliano-PC"
    QString conflictLabel = QStringLiteral("conflict");
    ConflictPolicy conflictPolicy = ConflictPolicy::KeepBoth;

    //! Deletions above this share of the synced files stop the run (e.g. a wrong root, lost permissions,
    //! an unmounted folder). Below minDeletionsForGuard deletions the guard never triggers.
    double maxDeleteShare = 0.5;
    int minDeletionsForGuard = 5;
    bool allowMassDelete = false;

    //! If not empty, only files matching one of these patterns are synced (e.g. "*.mscz").
    //! Other files are ignored on both sides: never uploaded, downloaded or deleted.
    QStringList includePatterns;

    //! File name patterns that are never synced. Names starting with '.' are always skipped.
    QStringList excludePatterns { QStringLiteral("*~"), QStringLiteral("*.tmp"), QStringLiteral("*.mscbackup") };

    //! Files open in the app (relative path): they are never replaced, renamed or deleted here, only uploaded
    //! once saved. Remote changes to them wait for the next run after they are closed.
    std::function<bool(const QString& relPath)> isOpenInApp;

    //! Files on demand: a Drive file that was never on this device is not downloaded, only listed (see
    //! SyncEngine::remoteFiles()), unless wanted() asks for it. Files on the device are synced as usual;
    //! SyncEngine::removeFromDevice() takes one off the device without deleting it on Drive.
    bool onDemand = false;
    std::function<bool(const QString& relPath)> wanted;

    //! A lock (contentRestrictions.readOnly) that this app may lift to upload, e.g. its own lock on a score
    //! with unsaved changes. Other locks are respected: the upload waits.
    std::function<bool(const DriveFile& file)> canLiftLock;
};

struct SyncReport
{
    int downloaded = 0;
    int uploaded = 0;
    int deletedLocal = 0;
    int deletedRemote = 0;
    QStringList conflicts;  // paths of the conflict copies made, or the conflicting paths with ReportOnly
    QStringList skipped;    // not synced this time, retried on the next run
    QStringList errors;
    bool aborted = false;

    bool ok() const { return errors.isEmpty() && !aborted; }
    int transfers() const { return downloaded + uploaded + deletedLocal + deletedRemote; }
};

//! One sync run between a local folder and a Drive folder.
//! Deleted files are recoverable: remote ones go to Drive's trash, local ones to <root>/.drivesync/trash.
class SyncEngine : public QObject
{
    Q_OBJECT

public:
    SyncEngine(DriveClient* client, SyncConfig config, QObject* parent = nullptr);

    void run();
    bool isRunning() const { return m_running; }

    const SyncConfig& config() const { return m_config; }
    QString manifestPath() const;
    QString localTrashPath() const;

    //! For tests
    void setClock(std::function<QDateTime()> clock) { m_clock = std::move(clock); }

    static QString conflictCopyName(const QString& path, const QString& label, const QString& device, const QDateTime& when);

    //! After a run: the Drive files and folders (relative paths), with their state on Drive
    const QHash<QString, DriveFile>& remoteFiles() const { return m_remote; }
    QStringList remoteFolders() const;

    //! Takes a file off the device without deleting it on Drive (on-demand mode). Refused if the file has
    //! changes that are not on Drive yet. Not while a run is going on.
    static bool removeFromDevice(const QString& localRoot, const QString& relPath, QString* error = nullptr);

    //! After a file or folder was moved or renamed on Drive: moves the local copy (if any) and its sync state,
    //! so that the next run neither downloads it again nor deletes it. Not while a run is going on.
    static bool relocateLocal(const QString& localRoot, const QString& fromRelPath, const QString& toRelPath,
                              QString* error = nullptr);

    //! After a run: Drive folder ids by relative path ("" = the root)
    const QHash<QString, QString>& remoteFolderIds() const { return m_remoteDirs; }

signals:
    void itemStarted(const QString& path, drivesync::SyncAction action);
    void finished(const drivesync::SyncReport& report);

private:
    struct LocalFile {
        QString md5;
        qint64 size = 0;
        qint64 mtimeMs = 0;
    };

    void listRemote();
    void listNextRemoteFolder();
    bool scanLocal();
    bool isExcluded(const QString& relPath) const;
    bool isSyncedFile(const QString& relPath) const;
    void planAndProcess();
    void processNext();
    void process(const PlannedItem& item);
    void done(const QString& path, const QString& error = QString());
    void finish();

    void download(const QString& path, const DriveFile& remote, std::function<void()> then);
    void uploadNew(const QString& path, std::function<void()> then);
    void uploadUpdate(const QString& path);
    void deleteLocal(const QString& path);
    void deleteRemote(const QString& path);
    void resolveConflict(const QString& path);
    void ensureRemoteDir(const QString& relDir, std::function<void(const QString& folderId, const DriveError& error)> then);

    void record(const QString& path, const DriveFile& remote);
    bool localUnchangedSinceScan(const QString& path) const;
    QString abs(const QString& relPath) const;
    QString uniqueConflictPath(const QString& path) const;

    DriveClient* m_client = nullptr;
    SyncConfig m_config;
    std::function<QDateTime()> m_clock = [] { return QDateTime::currentDateTime(); };

    bool m_running = false;
    SyncReport m_report;
    SyncManifest m_manifest;

    QHash<QString, DriveFile> m_remote;       // relPath -> file
    QHash<QString, QString> m_remoteDirs;     // relDir -> folder id ("" = root)
    QList<QPair<QString, QString> > m_folderQueue; // (folder id, relDir) still to list
    QSet<QString> m_duplicateRemote;
    QHash<QString, LocalFile> m_local;
    QList<PlannedItem> m_queue;
};
}
