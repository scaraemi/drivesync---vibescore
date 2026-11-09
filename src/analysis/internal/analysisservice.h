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

#include <optional>
#include <vector>

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "context/iglobalcontext.h"
#include "global/iinteractive.h"
#include "global/iglobalconfiguration.h"
#include "drive/idriveservice.h"

#include "../ianalysisservice.h"
#include "analysisdocument.h"

namespace mu::engraving {
class Score;
class Measure;
}

namespace mu::analysis {
class AnalysisService : public QObject, public IAnalysisService, public muse::Contextable, public muse::async::Asyncable
{
    Q_OBJECT

    muse::ContextInject<context::IGlobalContext> globalContext = { this };
    muse::ContextInject<muse::IInteractive> interactive = { this };
    muse::GlobalInject<muse::IGlobalConfiguration> globalConfiguration;
    muse::ContextInject<drive::IDriveService> driveService = { this };

public:
    explicit AnalysisService(const muse::modularity::ContextPtr& iocCtx);

    void init();

    bool isAvailable() const override;
    bool isActive() const override;
    void setActive(bool active) override;
    muse::async::Notification stateChanged() const override;
    muse::async::Notification repaintRequested() const override;

    Tool tool() const override;
    void setTool(Tool tool) override;
    QColor color() const override;
    void setColor(const QColor& color) override;
    double lineWidth() const override;
    void setLineWidth(double spatiums) override;
    QString symbol() const override;
    void setSymbol(const QString& symbol) override;

    QList<LayerInfo> layers() const override;
    void setLayerVisible(const QString& layerId, bool visible) override;

    bool canUndo() const override;
    bool canRedo() const override;
    void undo() override;
    void redo() override;
    bool hasSelection() const override;
    void deleteSelection() override;
    void clearMyLayer() override;

    muse::async::Notification textRequested() const override;
    void addText(const QString& text) override;

    bool pointerPress(const muse::PointF& pos) override;
    bool pointerMove(const muse::PointF& pos) override;
    bool pointerRelease(const muse::PointF& pos) override;
    bool keyPress(int key, Qt::KeyboardModifiers modifiers) override;

    void paint(muse::draw::Painter* painter) override;

    muse::Ret exportPdf() override;

private:
    struct Layer {
        QString id;
        QString path;
        AnalysisLayerFile file;
        bool mine = false;
        bool visible = true;
    };

    engraving::Score* score() const;
    double spatium() const;

    // The score open, its layers
    void onProjectChanged();
    void loadLayers();
    void reloadChangedLayers();
    Layer* myLayer();
    const Layer* myLayer() const;
    Layer& ensureMyLayer();
    QString authorId() const;
    QString authorName() const;
    void scheduleSave();
    void saveMyLayer();

    // Anchors: an object is placed relative to a measure (its start) and a staff (its top line)
    bool anchorAt(const muse::PointF& canvasPos, AnalysisObject& obj, QPointF& origin) const;
    std::optional<QPointF> originOf(const AnalysisObject& obj) const;
    std::vector<QPointF> canvasPoints(const AnalysisObject& obj) const;
    QRectF canvasBounds(const AnalysisObject& obj) const;
    QString hitTest(const muse::PointF& pos) const; // an object of my layer

    // Editing
    void pushUndo();
    void changed();
    QString newId() const;
    AnalysisObject::Shape shapeForTool() const;

    void paintObject(muse::draw::Painter* painter, const AnalysisObject& obj, bool selected) const;
    void paintLayers(muse::draw::Painter* painter, const QRectF& clip = QRectF()) const;

    bool m_active = false;
    Tool m_tool = Tool::Pen;
    QColor m_color = QColor(220, 30, 30);
    double m_width = 0.25;
    QString m_symbol = QStringLiteral("★");

    QString m_scorePath;
    std::vector<Layer> m_layers;
    std::vector<std::vector<AnalysisObject> > m_undo;
    std::vector<std::vector<AnalysisObject> > m_redo;

    // Pointer gestures
    bool m_drawing = false;
    AnalysisObject m_draft;
    QPointF m_draftOrigin;
    QString m_selectedId;
    bool m_moving = false;
    QPointF m_moveStart;
    std::vector<QPointF> m_moveOriginalPoints;
    muse::PointF m_textPos;

    QFileSystemWatcher m_watcher;
    QTimer m_saveTimer;
    QTimer m_reloadTimer;
    bool m_saving = false;

    muse::async::Notification m_stateChanged;
    muse::async::Notification m_repaintRequested;
    muse::async::Notification m_textRequested;
};
}
