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
