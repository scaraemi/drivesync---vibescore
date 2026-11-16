/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited and others
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

#include "global/modularity/imoduleinterface.h"
#include "global/types/retval.h"
#include "global/io/path.h"

#include "audio/common/soundfonttypes.h"

namespace muse::audio {
class ISoundFontController : MODULE_CONTEXT_INTERFACE
{
    INTERFACE_ID(ISoundFontController)

public:
    virtual ~ISoundFontController() = default;

    virtual void loadSoundFonts() = 0;

    virtual void addSoundFont(const synth::SoundFontUri& uri) = 0;

    //! Copies the SoundFont into the user's SoundFonts folder and loads it, without asking (an existing file of
    //! that name is replaced). Returns where it went.
    virtual RetVal<io::path_t> installSoundFont(const io::path_t& src) = 0;
};
}
