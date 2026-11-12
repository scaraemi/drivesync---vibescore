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
#include "drivemodule.h"

#include "modularity/ioc.h"

#include "ui/iuiactionsregister.h"

#include "internal/driveservice.h"
#include "internal/driveuiactions.h"

using namespace mu::drive;
using namespace muse;

std::string DriveModule::moduleName() const
{
    return "drive";
}

void DriveModule::registerExports()
{
    m_service = std::make_shared<DriveService>(iocContext());
    ioc()->registerExport<IDriveService>(moduleName(), m_service);
    m_uiActions = std::make_shared<DriveUiActions>(m_service, iocContext());
}

void DriveModule::resolveImports()
{
    auto ar = ioc()->resolve<muse::ui::IUiActionsRegister>(moduleName());
    if (ar) {
        ar->reg(m_uiActions);
    }
}

void DriveModule::onInit(const IApplication::RunMode& mode)
{
    if (mode != IApplication::RunMode::GuiApp) {
        return;
    }

    m_service->init();
    m_uiActions->init();
}

void DriveModule::onDeinit()
{
    if (m_service) {
        m_service->deinit();
    }
    m_uiActions.reset();
    m_service.reset();
}
