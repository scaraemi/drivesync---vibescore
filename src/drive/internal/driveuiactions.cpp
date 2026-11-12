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
#include "driveuiactions.h"

#include "context/uicontext.h"
#include "context/shortcutcontext.h"
#include "project/inotationproject.h"
#include "types/translatablestring.h"

#include "driveservice.h"

using namespace mu::drive;
using namespace muse;
using namespace muse::ui;
using namespace muse::actions;

static const ActionCode SAVE_TO_DRIVE_CODE("drive-save-to-drive");

const UiActionList DriveUiActions::m_actions = {
    UiAction(SAVE_TO_DRIVE_CODE,
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_ANY,
             TranslatableString("action", "Save to &Drive…"),
             TranslatableString("action", "Save a copy of the score in the shared Drive folder")
             ),
};

DriveUiActions::DriveUiActions(std::shared_ptr<DriveService> service, const modularity::ContextPtr& iocCtx)
    : muse::Contextable(iocCtx), m_service(service)
{
}

void DriveUiActions::init()
{
    auto update = [this]() { m_actionEnabledChanged.send({ SAVE_TO_DRIVE_CODE }); };
    m_service->statusChanged().onNotify(this, update);
    if (globalContext()) {
        globalContext()->currentProjectChanged().onNotify(this, update);
    }
}

const UiActionList& DriveUiActions::actionsList() const
{
    return m_actions;
}

bool DriveUiActions::actionEnabled(const UiAction& act) const
{
    if (act.code != SAVE_TO_DRIVE_CODE) {
        return false;
    }
    if (!m_service->isAvailable() || !globalContext() || !globalContext()->currentProject()) {
        return false;
    }
    // Problems (not signed in, a score not saved yet, one already in Drive) are explained when it is chosen
    return true;
}

bool DriveUiActions::actionChecked(const UiAction&) const
{
    return false;
}

async::Channel<ActionCodeList> DriveUiActions::actionEnabledChanged() const
{
    return m_actionEnabledChanged;
}

async::Channel<ActionCodeList> DriveUiActions::actionCheckedChanged() const
{
    return m_actionCheckedChanged;
}
