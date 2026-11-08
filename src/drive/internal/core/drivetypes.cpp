// SPDX-License-Identifier: GPL-3.0-only
#include "drivetypes.h"

#include <QJsonArray>

using namespace drivesync;

QString DriveError::toString() const
{
    QString s = message.isEmpty() ? reason : message;
    if (httpStatus > 0) {
        s = QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(s);
    }
    return s;
}

bool DriveFile::isFolder() const
{
    return mimeType == FOLDER_MIME;
}

bool DriveFile::isGoogleNative() const
{
    return !isFolder() && mimeType.startsWith(QLatin1String("application/vnd.google-apps."));
}

DriveFile DriveFile::fromJson(const QJsonObject& obj)
{
    DriveFile f;
    f.id = obj.value("id").toString();
    f.name = obj.value("name").toString();
    f.mimeType = obj.value("mimeType").toString();
    f.md5 = obj.value("md5Checksum").toString();
    f.headRevisionId = obj.value("headRevisionId").toString();
    f.size = obj.value("size").toString().toLongLong(); // Drive sends int64 as string
    f.trashed = obj.value("trashed").toBool();
    f.modifiedTime = QDateTime::fromString(obj.value("modifiedTime").toString(), Qt::ISODateWithMs);
    f.lastModifyingUser = obj.value("lastModifyingUser").toObject().value("displayName").toString();
    f.lastModifiedByMe = obj.value("lastModifyingUser").toObject().value("me").toBool();

    for (const QJsonValue& p : obj.value("parents").toArray()) {
        f.parents << p.toString();
    }

    for (const QJsonValue& r : obj.value("contentRestrictions").toArray()) {
        const QJsonObject restriction = r.toObject();
        if (restriction.value("readOnly").toBool()) {
            f.locked = true;
            f.lockReason = restriction.value("reason").toString();
            f.lockedBy = restriction.value("restrictingUser").toObject().value("displayName").toString();
            f.lockedByMe = restriction.value("restrictingUser").toObject().value("me").toBool();
            f.lockTime = QDateTime::fromString(restriction.value("restrictionTime").toString(), Qt::ISODateWithMs);
        }
    }

    return f;
}
