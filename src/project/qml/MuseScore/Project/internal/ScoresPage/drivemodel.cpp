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
#include "drivemodel.h"

#include <QFileInfo>
#include <algorithm>
#include <QLocale>

#include "translation.h"
#include "log.h"

using namespace mu::project;
using namespace mu::drive;
using namespace muse;

DriveModel::DriveModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

void DriveModel::load()
{
    if (!driveService()) {
        return;
    }

    m_lastState = state();
    driveService()->itemsChanged().onNotify(this, [this]() {
        // The folder shown may be gone
        if (!m_currentFolder.isEmpty()) {
            bool exists = false;
            for (const drive::IDriveService::Item& item : driveService()->items()) {
                if (item.isFolder && item.path == m_currentFolder) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                m_currentFolder.clear();
            }
        }
        emit itemsChanged();
    });
    driveService()->statusChanged().onNotify(this, [this]() {
        const QString newState = state();
        const bool finished = m_lastState == "syncing" && newState != "syncing";
        m_lastState = newState;
        emit statusChanged();
        if (finished) {
            emit syncFinished();
        }
    });
}

bool DriveModel::isAvailable() const
{
    return driveService() && driveService()->isAvailable();
}

QString DriveModel::state() const
{
    if (!driveService()) {
        return "notSignedIn";
    }

    switch (driveService()->status().state) {
    case IDriveService::State::NotSignedIn: return "notSignedIn";
    case IDriveService::State::SigningIn: return "signingIn";
    case IDriveService::State::NoFolder: return "noFolder";
    case IDriveService::State::Idle: return "idle";
    case IDriveService::State::Syncing: return "syncing";
    }
    return "notSignedIn";
}

QString DriveModel::statusText() const
{
    if (!driveService()) {
        return QString();
    }

    const IDriveService::Status status = driveService()->status();
    switch (status.state) {
    case IDriveService::State::NotSignedIn:
        return muse::qtrc("project", "Sign in with the Google account that has the shared folder of scores.");
    case IDriveService::State::SigningIn:
        return muse::qtrc("project", "Finish the sign-in in the browser, then come back here.");
    case IDriveService::State::NoFolder:
        return muse::qtrc("project", "Paste the link of the Drive folder of the scores.");
    case IDriveService::State::Syncing:
        if (!status.fetching.isEmpty()) {
            //: %1 is the name of a score
            return muse::qtrc("project", "Downloading “%1”…").arg(QFileInfo(status.fetching).completeBaseName());
        }
        return muse::qtrc("project", "Syncing…");
    case IDriveService::State::Idle:
        break;
    }

    if (!status.lastSync.isValid()) {
        return muse::qtrc("project", "Not synced yet");
    }

    const QString time = QLocale().toString(status.lastSync.time(), QLocale::ShortFormat);
    if (!status.lastSyncOk || !status.problems.isEmpty()) {
        //: %1 is a time
        return muse::qtrc("project", "Synced at %1, with problems").arg(time);
    }
    //: %1 is a time
    return muse::qtrc("project", "Synced at %1").arg(time);
}

QStringList DriveModel::problems() const
{
    if (!driveService()) {
        return {};
    }

    const IDriveService::Status status = driveService()->status();
    QStringList result;
    for (const QString& conflict : status.conflicts) {
        //: %1 is the name of a score. Two people changed it at the same time: both versions are kept
        result << muse::qtrc("project", "Changed here and on Drive: your version is “%1”").arg(conflict);
    }
    result << status.problems;
    return result;
}

QString DriveModel::rootPath() const
{
    return driveService() ? driveService()->rootPath().toQString() : QString();
}

void DriveModel::signIn()
{
    if (driveService()) {
        driveService()->signIn();
    }
}

void DriveModel::cancelSignIn()
{
    if (driveService()) {
        driveService()->cancelSignIn();
    }
}

void DriveModel::signOut()
{
    if (driveService()) {
        driveService()->signOut();
    }
}

QString DriveModel::setFolder(const QString& linkOrId)
{
    if (!driveService()) {
        return QString();
    }

    const Ret ret = driveService()->setFolder(linkOrId);
    return ret ? QString() : QString::fromStdString(ret.text());
}

void DriveModel::syncNow()
{
    if (driveService()) {
        driveService()->sync();
    }
}

void DriveModel::copyToDrive(const QString& scorePath)
{
    if (!driveService()) {
        return;
    }

    const RetVal<io::path_t> copy = driveService()->copyToDrive(scorePath);
    if (!copy.ret) {
        interactive()->error(muse::trc("project", "The score could not be copied to Drive."), IInteractive::Text(copy.ret.text()));
        return;
    }

    interactive()->info(muse::trc("project", "Copied to Drive"),
                        IInteractive::Text(muse::trc("project", "The score is in the Drive tab, and it is being uploaded.")));
}

QString DriveModel::currentFolder() const
{
    return m_currentFolder;
}

QString DriveModel::currentFolderName() const
{
    return m_currentFolder.mid(m_currentFolder.lastIndexOf('/') + 1);
}

QString DriveModel::searchText() const
{
    return m_searchText;
}

void DriveModel::setSearchText(const QString& text)
{
    if (m_searchText == text) {
        return;
    }
    m_searchText = text;
    emit itemsChanged();
}

static QString sizeText(qint64 bytes)
{
    return bytes < 1024 * 1024 ? QString("%1 KB").arg(qMax<qint64>(1, bytes / 1024))
           : QString("%1 MB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
}

QVariantMap DriveModel::toMap(const IDriveService::Item& item) const
{
    QVariantMap map;
    map["path"] = item.path;
    map["name"] = item.isFolder ? item.path.mid(item.path.lastIndexOf('/') + 1) : QFileInfo(item.path).completeBaseName();
    map["isFolder"] = item.isFolder;
    map["onDevice"] = item.onDevice;
    map["notUploaded"] = item.notUploaded;
    map["locked"] = !item.lockedBy.isEmpty();
    map["lockedHere"] = item.lockedHere;
    map["lockStale"] = item.lockStale;

    QStringList parts;
    if (!m_searchText.isEmpty() && item.path.contains('/')) {
        parts << item.path.left(item.path.lastIndexOf('/'));
    }
    if (item.isFolder) {
        map["subtitle"] = parts.join(QStringLiteral(" · "));
        map["lockText"] = QString();
        return map;
    }

    if (item.notUploaded) {
        parts << muse::qtrc("project", "Not on Drive yet");
    } else {
        if (item.modified.isValid()) {
            const QString when = QLocale().toString(item.modified, QLocale::ShortFormat);
            //: %1 is a person, %2 a date and time
            parts << (item.modifiedBy.isEmpty() ? when : muse::qtrc("project", "%1, %2").arg(item.modifiedBy, when));
        }
        parts << sizeText(item.size);
        parts << (item.onDevice ? muse::qtrc("project", "On this device") : muse::qtrc("project", "On Drive"));
    }
    map["subtitle"] = parts.join(QStringLiteral(" · "));

    QString lockText;
    if (item.lockedHere) {
        lockText = muse::qtrc("project", "Being edited here");
    } else if (!item.lockedBy.isEmpty()) {
        const QString since = QLocale().toString(item.lockedSince, QLocale::ShortFormat);
        //: %1 is a person, %2 e.g. "Unsaved changes on Tablet", %3 a date and time
        lockText = item.lockStale
                   ? muse::qtrc("project", "Was being edited by %1 (%2), since %3: not renewed").arg(item.lockedBy, item.lockReason, since)
                   : muse::qtrc("project", "Being edited by %1 (%2), since %3").arg(item.lockedBy, item.lockReason, since);
    }
    map["lockText"] = lockText;
    return map;
}

QVariantList DriveModel::items() const
{
    if (!driveService()) {
        return {};
    }

    QList<IDriveService::Item> shown;
    const QString search = m_searchText.trimmed();
    for (const IDriveService::Item& item : driveService()->items()) {
        if (!search.isEmpty()) {
            if (!item.isFolder && QFileInfo(item.path).completeBaseName().contains(search, Qt::CaseInsensitive)) {
                shown << item;
            }
            continue;
        }
        const int slash = item.path.lastIndexOf('/');
        const QString parent = slash < 0 ? QString() : item.path.left(slash);
        if (parent == m_currentFolder) {
            shown << item;
        }
    }

    // Folders first, then by name
    std::sort(shown.begin(), shown.end(), [](const IDriveService::Item& a, const IDriveService::Item& b) {
        if (a.isFolder != b.isFolder) {
            return a.isFolder;
        }
        return QString::localeAwareCompare(a.path, b.path) < 0;
    });

    QVariantList result;
    for (const IDriveService::Item& item : shown) {
        result << toMap(item);
    }
    return result;
}

void DriveModel::openFolder(const QString& path)
{
    m_currentFolder = path;
    m_searchText.clear();
    emit itemsChanged();
}

void DriveModel::goUp()
{
    const int slash = m_currentFolder.lastIndexOf('/');
    openFolder(slash < 0 ? QString() : m_currentFolder.left(slash));
}

void DriveModel::openItem(const QString& path)
{
    if (!driveService()) {
        return;
    }
    for (const IDriveService::Item& item : driveService()->items()) {
        if (item.path == path) {
            if (item.isFolder) {
                openFolder(path);
            } else {
                driveService()->openScore(path);
            }
            return;
        }
    }
}

void DriveModel::removeFromDevice(const QString& path)
{
    if (!driveService()) {
        return;
    }
    const Ret ret = driveService()->removeFromDevice(path);
    if (!ret) {
        interactive()->info(muse::trc("project", "The score was not removed from this device"), IInteractive::Text(ret.text()));
    }
}

void DriveModel::deleteFromDrive(const QString& path)
{
    if (!driveService()) {
        return;
    }

    IInteractive::ButtonData cancel = interactive()->buttonData(IInteractive::Button::Cancel);
    cancel.role = IInteractive::ButtonRole::RejectRole;
    IInteractive::ButtonData del(int(IInteractive::Button::CustomButton) + 1, muse::trc("global", "Delete"), true);
    del.role = IInteractive::ButtonRole::AcceptRole;

    //: %1 is the name of a score
    interactive()->question(muse::qtrc("project", "Delete “%1” for everyone?").arg(QFileInfo(path).completeBaseName()).toStdString(),
                            IInteractive::Text(muse::trc("project", "It goes to the trash of Google Drive, where it can be "
                                                                    "restored for 30 days.")),
                            { cancel, del }, cancel.btn)
    .onResolve(this, [this, path, delBtn = del.btn](const IInteractive::Result& result) {
        if (result.isButton(delBtn)) {
            driveService()->deleteFromDrive(path);
        }
    });
}

void DriveModel::unlock(const QString& path)
{
    if (driveService()) {
        driveService()->unlock(path);
    }
}
