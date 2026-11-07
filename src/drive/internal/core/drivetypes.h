// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace drivesync {
struct DriveError
{
    int httpStatus = 0;   // 0 = no HTTP response (network or local error)
    QString reason;       // Google's error reason, e.g. "rateLimitExceeded", or a local code
    QString message;

    QString toString() const;
};

template<typename T>
struct Result
{
    std::optional<T> value;
    DriveError error;

    bool ok() const { return value.has_value(); }
    static Result success(T v) { Result r; r.value = std::move(v); return r; }
    static Result failure(DriveError e) { Result r; r.error = std::move(e); return r; }
};

struct DriveFile
{
    QString id;
    QString name;
    QString mimeType;
    QString md5;              // md5Checksum, only for binary files
    QString headRevisionId;   // only for binary files
    QString lastModifyingUser;
    QStringList parents;
    QDateTime modifiedTime;
    qint64 size = 0;
    bool trashed = false;
    bool locked = false;      // contentRestrictions.readOnly
    QString lockReason;

    bool isFolder() const;
    bool isGoogleNative() const; // Docs, Sheets, ...: no binary content to sync

    static DriveFile fromJson(const QJsonObject& obj);
};

inline const QString FOLDER_MIME = QStringLiteral("application/vnd.google-apps.folder");
}
