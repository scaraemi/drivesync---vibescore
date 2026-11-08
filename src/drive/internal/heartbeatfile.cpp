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
#include "heartbeatfile.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

using namespace mu::drive;

HeartbeatFile::HeartbeatFile(QString path)
    : m_path(std::move(path))
{
    m_timer.setInterval(REFRESH_MS);
    QObject::connect(&m_timer, &QTimer::timeout, [this]() { touch(); });
}

HeartbeatFile::~HeartbeatFile()
{
    release();
}

bool HeartbeatFile::isHeldByAnyone(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() && qAbs(info.lastModified().msecsTo(QDateTime::currentDateTime())) < STALE_MS;
}

bool HeartbeatFile::acquire()
{
    if (m_held) {
        return true;
    }

    QDir().mkpath(QFileInfo(m_path).absolutePath());
    for (int attempt = 0; attempt < 2; ++attempt) {
        QFile file(m_path);
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            file.write(QByteArray::number(QCoreApplication::applicationPid()));
            file.close();
            m_held = true;
            m_timer.start();
            return true;
        }
        if (isHeldByAnyone(m_path)) {
            return false;
        }
        QFile::remove(m_path); // left by a process that is gone
    }
    return false;
}

void HeartbeatFile::release()
{
    if (!m_held) {
        return;
    }
    m_timer.stop();
    QFile::remove(m_path);
    m_held = false;
}

bool HeartbeatFile::isHeld() const
{
    return m_held;
}

void HeartbeatFile::touch()
{
    QFile file(m_path);
    if (file.open(QIODevice::Append)) {
        file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
        file.close();
    }
}
