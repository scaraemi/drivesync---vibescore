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
#pragma once

#include <QString>

namespace mu::drive {
//! Writes a crash report (signal, fault address, the stack as library + offset) next to the app's logs when the
//! app crashes, then lets the system handle the crash as usual. On the next start the app offers to send it.
//! Android and Linux; elsewhere it does nothing.
class CrashHandler
{
public:
    //! Reports go to <logsDir>/crash_<yyMMdd_HHmmss>.txt
    static void install(const QString& logsDir);
};
}
