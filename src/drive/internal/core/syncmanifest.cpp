// SPDX-License-Identifier: GPL-3.0-only
#include "syncmanifest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

using namespace drivesync;

static constexpr int FORMAT_VERSION = 1;

bool SyncManifest::load(const QString& filePath)
{
    remoteRootId.clear();
    entries.clear();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value("version").toInt() != FORMAT_VERSION) {
        return false;
    }

    remoteRootId = root.value("remoteRootId").toString();
    const QJsonObject files = root.value("files").toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        ManifestEntry e;
        e.fileId = o.value("id").toString();
        e.baseRevision = o.value("rev").toString();
        e.baseMd5 = o.value("md5").toString();
        e.localSize = o.value("size").toVariant().toLongLong();
        e.localMtimeMs = o.value("mtime").toVariant().toLongLong();
        entries.insert(it.key(), e);
    }
    return true;
}

bool SyncManifest::save(const QString& filePath) const
{
    QJsonObject files;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const ManifestEntry& e = it.value();
        files.insert(it.key(), QJsonObject {
            { "id", e.fileId },
            { "rev", e.baseRevision },
            { "md5", e.baseMd5 },
            { "size", QString::number(e.localSize) },
            { "mtime", QString::number(e.localMtimeMs) },
        });
    }

    const QJsonObject root {
        { "version", FORMAT_VERSION },
        { "remoteRootId", remoteRootId },
        { "files", files },
    };

    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}
