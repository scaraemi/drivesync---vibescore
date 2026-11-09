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
#include "analysistoolbarmodel.h"

#include "translation.h"

using namespace mu::notation;
using namespace mu::analysis;
using namespace muse;

AnalysisToolbarModel::AnalysisToolbarModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

void AnalysisToolbarModel::load()
{
    if (!analysisService()) {
        return;
    }
    analysisService()->stateChanged().onNotify(this, [this]() {
        emit stateChanged();
    });
    analysisService()->textRequested().onNotify(this, [this]() {
        emit textRequested();
    });
    emit stateChanged();
}

bool AnalysisToolbarModel::isAvailable() const
{
    return analysisService() && analysisService()->isAvailable();
}

bool AnalysisToolbarModel::isActive() const
{
    return analysisService() && analysisService()->isActive();
}

int AnalysisToolbarModel::tool() const
{
    return analysisService() ? int(analysisService()->tool()) : 0;
}

QColor AnalysisToolbarModel::color() const
{
    return analysisService() ? analysisService()->color() : QColor();
}

double AnalysisToolbarModel::lineWidth() const
{
    return analysisService() ? analysisService()->lineWidth() : 0.25;
}

QString AnalysisToolbarModel::symbol() const
{
    return analysisService() ? analysisService()->symbol() : QString();
}

bool AnalysisToolbarModel::canUndo() const
{
    return analysisService() && analysisService()->canUndo();
}

bool AnalysisToolbarModel::canRedo() const
{
    return analysisService() && analysisService()->canRedo();
}

bool AnalysisToolbarModel::hasSelection() const
{
    return analysisService() && analysisService()->hasSelection();
}

QVariantList AnalysisToolbarModel::layers() const
{
    QVariantList result;
    if (!analysisService()) {
        return result;
    }
    for (const IAnalysisService::LayerInfo& layer : analysisService()->layers()) {
        result << QVariantMap {
            { "id", layer.id },
            //: In the list of analysis layers: one's own
            { "author", layer.mine ? muse::qtrc("notation", "You") : layer.author },
            { "mine", layer.mine },
            { "visible", layer.visible },
            { "count", layer.objectCount },
        };
    }
    return result;
}

QVariantList AnalysisToolbarModel::colors() const
{
    return { QColor(220, 30, 30), QColor(30, 100, 220), QColor(20, 150, 60), QColor(240, 140, 0), QColor(140, 50, 200),
             QColor(30, 30, 30) };
}

QStringList AnalysisToolbarModel::symbols() const
{
    return { QStringLiteral("★"), QStringLiteral("✓"), QStringLiteral("✗"), QStringLiteral("!"), QStringLiteral("?"),
             QStringLiteral("↑"), QStringLiteral("↓"), QStringLiteral("→"), QStringLiteral("♯"), QStringLiteral("♭"),
             QStringLiteral("♮"), QStringLiteral("𝄐"), QStringLiteral("◯"), QStringLiteral("△") };
}

void AnalysisToolbarModel::toggle()
{
    if (analysisService()) {
        analysisService()->setActive(!analysisService()->isActive());
    }
}

void AnalysisToolbarModel::setTool(int tool)
{
    if (analysisService()) {
        analysisService()->setTool(IAnalysisService::Tool(tool));
    }
}

void AnalysisToolbarModel::setColor(const QColor& color)
{
    if (analysisService()) {
        analysisService()->setColor(color);
    }
}

void AnalysisToolbarModel::setLineWidth(double spatiums)
{
    if (analysisService()) {
        analysisService()->setLineWidth(spatiums);
    }
}

void AnalysisToolbarModel::setSymbol(const QString& symbol)
{
    if (analysisService()) {
        analysisService()->setSymbol(symbol);
    }
}

void AnalysisToolbarModel::undo()
{
    if (analysisService()) {
        analysisService()->undo();
    }
}

void AnalysisToolbarModel::redo()
{
    if (analysisService()) {
        analysisService()->redo();
    }
}

void AnalysisToolbarModel::deleteSelection()
{
    if (analysisService()) {
        analysisService()->deleteSelection();
    }
}

void AnalysisToolbarModel::clearMine()
{
    if (!analysisService()) {
        return;
    }

    IInteractive::ButtonData cancel = interactive()->buttonData(IInteractive::Button::Cancel);
    cancel.role = IInteractive::ButtonRole::RejectRole;
    IInteractive::ButtonData clear(int(IInteractive::Button::CustomButton) + 1, muse::trc("notation", "Clear"), true);
    clear.role = IInteractive::ButtonRole::AcceptRole;

    interactive()->question(muse::trc("notation", "Clear your analysis of this score?"),
                            IInteractive::Text(muse::trc("notation", "The others’ layers are not touched. You can undo it.")),
                            { cancel, clear }, cancel.btn)
    .onResolve(this, [this, clearBtn = clear.btn](const IInteractive::Result& result) {
        if (result.isButton(clearBtn)) {
            analysisService()->clearMyLayer();
        }
    });
}

void AnalysisToolbarModel::setLayerVisible(const QString& layerId, bool visible)
{
    if (analysisService()) {
        analysisService()->setLayerVisible(layerId, visible);
    }
}

void AnalysisToolbarModel::addText(const QString& text)
{
    if (analysisService()) {
        analysisService()->addText(text);
    }
}

void AnalysisToolbarModel::exportPdf()
{
    if (!analysisService()) {
        return;
    }
    const Ret ret = analysisService()->exportPdf();
    if (!ret && ret.code() != int(Ret::Code::Cancel)) {
        interactive()->error(muse::trc("notation", "The PDF was not exported"), IInteractive::Text(ret.text()));
    }
}
