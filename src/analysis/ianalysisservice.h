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

#include <QColor>
#include <QList>
#include <QString>

#include "global/modularity/imoduleinterface.h"
#include "global/async/notification.h"
#include "global/types/ret.h"
#include "draw/types/geometry.h"

namespace muse::draw {
class Painter;
}

namespace mu::analysis {
//! "Analysis": circles, texts, freehand drawings, arrows and symbols over the open score. Each object is anchored to
//! a measure and a staff, so it follows them when the score is laid out again (another page format, phone or PC).
//! Off: nothing is shown; on again: everything is back. Each person draws on a layer of their own, kept in a file
//! next to the score ("<score>.<author>.vsanalysis") and shared with the score's folder (Google Drive); the others'
//! layers can be shown or hidden. With analysis on, the score can be exported to PDF with the layers shown.
//!
//! Header only for the users of the service (the notation view): they resolve it at run time.
class IAnalysisService : MODULE_CONTEXT_INTERFACE
{
    INTERFACE_ID(IAnalysisService)

public:
    virtual ~IAnalysisService() = default;

    enum class Tool {
        Select,     // move one's own objects, delete the selected one
        Pen,        // freehand
        Line,
        Arrow,
        Circle,     // ellipse in the dragged rectangle
        Text,
        Symbol,
        Eraser,     // tap an object of one's own layer to delete it
    };

    struct LayerInfo {
        QString id;
        QString author;
        bool mine = false;
        bool visible = true;
        int objectCount = 0;
    };

    //! A score is open
    virtual bool isAvailable() const = 0;
    virtual bool isActive() const = 0;
    virtual void setActive(bool active) = 0;
    //! Anything shown in the toolbar changed (active, tool, colour, layers, undo)
    virtual muse::async::Notification stateChanged() const = 0;
    //! The view should paint again
    virtual muse::async::Notification repaintRequested() const = 0;

    virtual Tool tool() const = 0;
    virtual void setTool(Tool tool) = 0;
    virtual QColor color() const = 0;
    virtual void setColor(const QColor& color) = 0;
    //! Line width in staff spaces
    virtual double lineWidth() const = 0;
    virtual void setLineWidth(double spatiums) = 0;
    virtual QString symbol() const = 0;
    virtual void setSymbol(const QString& symbol) = 0;

    virtual QList<LayerInfo> layers() const = 0;
    virtual void setLayerVisible(const QString& layerId, bool visible) = 0;

    virtual bool canUndo() const = 0;
    virtual bool canRedo() const = 0;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual bool hasSelection() const = 0;
    virtual void deleteSelection() = 0;
    //! Everything in one's own layer
    virtual void clearMyLayer() = 0;

    //! The text tool asks for the text: the UI answers with addText() (or nothing, to cancel)
    virtual muse::async::Notification textRequested() const = 0;
    virtual void addText(const QString& text) = 0;

    // From the notation view, in score (logical) coordinates; true when the analysis took the event
    virtual bool pointerPress(const muse::PointF& pos) = 0;
    virtual bool pointerMove(const muse::PointF& pos) = 0;
    virtual bool pointerRelease(const muse::PointF& pos) = 0;
    virtual bool keyPress(int key, Qt::KeyboardModifiers modifiers) = 0;

    //! Paints the visible layers (only while active), in score coordinates
    virtual void paint(muse::draw::Painter* painter) = 0;

    //! The score as PDF with the visible layers: asks where to save it (desktop) or shares it (Android)
    virtual muse::Ret exportPdf() = 0;
};
}
