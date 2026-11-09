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
#include <QObject>
#include <QVariantList>
#include <qqmlintegration.h>

#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "global/iinteractive.h"

#include "analysis/ianalysisservice.h"

namespace mu::notation {
//! The analysis toolbar over the notation view: on/off, tools, colours, layers, PDF export.
//! Empty (isAvailable false) when the app has no analysis module or no saved score is open.
class AnalysisToolbarModel : public QObject, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    Q_PROPERTY(bool isAvailable READ isAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool isActive READ isActive NOTIFY stateChanged)
    Q_PROPERTY(int tool READ tool NOTIFY stateChanged)
    Q_PROPERTY(QColor color READ color NOTIFY stateChanged)
    Q_PROPERTY(double lineWidth READ lineWidth NOTIFY stateChanged)
    Q_PROPERTY(QString symbol READ symbol NOTIFY stateChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY stateChanged)
    Q_PROPERTY(QVariantList layers READ layers NOTIFY stateChanged)
    Q_PROPERTY(QVariantList colors READ colors CONSTANT)
    Q_PROPERTY(QStringList symbols READ symbols CONSTANT)

    QML_ELEMENT

    muse::ContextInject<analysis::IAnalysisService> analysisService = { this };
    muse::ContextInject<muse::IInteractive> interactive = { this };

public:
    // As IAnalysisService::Tool
    enum Tool {
        Select = 0, Pen, Line, Arrow, Circle, Text, Symbol, Eraser
    };
    Q_ENUM(Tool)

    explicit AnalysisToolbarModel(QObject* parent = nullptr);

    Q_INVOKABLE void load();

    bool isAvailable() const;
    bool isActive() const;
    int tool() const;
    QColor color() const;
    double lineWidth() const;
    QString symbol() const;
    bool canUndo() const;
    bool canRedo() const;
    bool hasSelection() const;
    QVariantList layers() const;
    QVariantList colors() const;
    QStringList symbols() const;

    Q_INVOKABLE void toggle();
    Q_INVOKABLE void setTool(int tool);
    Q_INVOKABLE void setColor(const QColor& color);
    Q_INVOKABLE void setLineWidth(double spatiums);
    Q_INVOKABLE void setSymbol(const QString& symbol);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void clearMine();
    Q_INVOKABLE void setLayerVisible(const QString& layerId, bool visible);
    Q_INVOKABLE void addText(const QString& text);
    Q_INVOKABLE void exportPdf();

signals:
    void stateChanged();
    //! The text tool: the UI asks for the text, then calls addText()
    void textRequested();
};
}
