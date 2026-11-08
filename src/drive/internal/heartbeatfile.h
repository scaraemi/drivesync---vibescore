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
#include <QTimer>

namespace mu::drive {
//! A marker file shared by all the app's processes (MuseScore desktop opens every score in its own process):
//! it exists and is kept fresh while its owner is alive. A crashed owner's file goes stale and is ignored.
//! Used for "a sync is running" and "this score is open in some window".
class HeartbeatFile
{
public:
    explicit HeartbeatFile(QString path);
    ~HeartbeatFile();

    //! Creates the file if no live owner has it (atomically). False if it is held elsewhere.
    bool acquire();
    void release();
    bool isHeld() const;

    //! True if a live process (this one or another) holds the file at path
    static bool isHeldByAnyone(const QString& path);

    static constexpr int REFRESH_MS = 20 * 1000;
    static constexpr int STALE_MS = 75 * 1000;

private:
    void touch();

    QString m_path;
    QTimer m_timer;
    bool m_held = false;
};
}
