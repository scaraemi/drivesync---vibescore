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
#include "analysismodule.h"

#include "modularity/ioc.h"

#include "internal/analysisservice.h"

using namespace mu::analysis;
using namespace muse;

std::string AnalysisModule::moduleName() const
{
    return "analysis";
}

void AnalysisModule::registerExports()
{
    m_service = std::make_shared<AnalysisService>(iocContext());
    ioc()->registerExport<IAnalysisService>(moduleName(), m_service);
}

void AnalysisModule::onInit(const IApplication::RunMode& mode)
{
    if (mode != IApplication::RunMode::GuiApp) {
        return;
    }
    m_service->init();
}

void AnalysisModule::onDeinit()
{
    m_service.reset();
}
