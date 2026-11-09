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
#include "analysisservice.h"

#include <cmath>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLineF>
#include <QPageSize>
#include <QPdfWriter>
#include <QSettings>
#include <QUuid>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#endif

#include "draw/painter.h"
#include "draw/types/pen.h"
#include "draw/types/brush.h"
#include "draw/types/font.h"

#include "engraving/dom/score.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/system.h"
#include "engraving/dom/page.h"
#include "engraving/types/fraction.h"

#include "notation/inotation.h"
#include "notation/inotationelements.h"
#include "notation/inotationpainting.h"
#include "project/inotationproject.h"

#include "translation.h"
#include "log.h"

using namespace mu::analysis;
using namespace muse;

static constexpr int SAVE_DELAY_MS = 800;
static constexpr int SYNC_AFTER_SAVE_MS = 2500;
static constexpr int RELOAD_DELAY_MS = 500;
static constexpr double PEN_MIN_STEP_SP = 0.08;   // freehand: points closer than this are skipped
static constexpr double MIN_SHAPE_SP = 0.3;       // smaller lines/circles are taps, not shapes
static constexpr double HIT_TOLERANCE_SP = 0.6;
static constexpr double SYMBOL_SIZE_SP = 2.5;
static constexpr double TEXT_SIZE_SP = 1.8;

static const QString AUTHOR_ID_KEY = QStringLiteral("analysis/authorId");
static const QString AUTHOR_NAME_KEY = QStringLiteral("analysis/authorName");
static const QString HIDDEN_LAYERS_KEY = QStringLiteral("analysis/hiddenLayers");

static QPointF toQ(const PointF& p)
{
    return QPointF(p.x(), p.y());
}

AnalysisService::AnalysisService(const modularity::ContextPtr& iocCtx)
    : Contextable(iocCtx)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(SAVE_DELAY_MS);
    connect(&m_saveTimer, &QTimer::timeout, this, &AnalysisService::saveMyLayer);

    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(RELOAD_DELAY_MS);
    connect(&m_reloadTimer, &QTimer::timeout, this, &AnalysisService::reloadChangedLayers);

    // Layers written by the sync (the others') or by another window
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this]() { m_reloadTimer.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this]() { m_reloadTimer.start(); });
}

void AnalysisService::init()
{
    globalContext()->currentProjectChanged().onNotify(this, [this]() {
        onProjectChanged();
    });
    onProjectChanged();
}

mu::engraving::Score* AnalysisService::score() const
{
    notation::INotationPtr notation = globalContext()->currentNotation();
    return notation ? notation->elements()->msScore() : nullptr;
}

double AnalysisService::spatium() const
{
    const engraving::Score* s = score();
    return s ? s->style().spatium() : 25.0;
}

// ---------------------------------------------------------------------------------------------------------------------
// State

bool AnalysisService::isAvailable() const
{
    return !m_scorePath.isEmpty() && score();
}

bool AnalysisService::isActive() const
{
    return m_active && isAvailable();
}

void AnalysisService::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    m_drawing = false;
    m_moving = false;
    m_selectedId.clear();
    if (!active && m_saveTimer.isActive()) {
        m_saveTimer.stop();
        saveMyLayer();
    }
    m_stateChanged.notify();
    m_repaintRequested.notify();
}

async::Notification AnalysisService::stateChanged() const
{
    return m_stateChanged;
}

async::Notification AnalysisService::repaintRequested() const
{
    return m_repaintRequested;
}

IAnalysisService::Tool AnalysisService::tool() const
{
    return m_tool;
}

void AnalysisService::setTool(Tool tool)
{
    m_tool = tool;
    if (tool != Tool::Select) {
        m_selectedId.clear();
        m_repaintRequested.notify();
    }
    m_stateChanged.notify();
}

QColor AnalysisService::color() const
{
    return m_color;
}

void AnalysisService::setColor(const QColor& color)
{
    m_color = color;
    m_stateChanged.notify();
}

double AnalysisService::lineWidth() const
{
    return m_width;
}

void AnalysisService::setLineWidth(double spatiums)
{
    m_width = spatiums;
    m_stateChanged.notify();
}

QString AnalysisService::symbol() const
{
    return m_symbol;
}

void AnalysisService::setSymbol(const QString& symbol)
{
    m_symbol = symbol;
    m_tool = Tool::Symbol;
    m_stateChanged.notify();
}

// ---------------------------------------------------------------------------------------------------------------------
// Layers

QString AnalysisService::authorName() const
{
    if (driveService() && !driveService()->accountName().isEmpty()) {
        return driveService()->accountName();
    }
    const QString saved = QSettings().value(AUTHOR_NAME_KEY).toString();
    if (!saved.isEmpty()) {
        return saved;
    }
    const QString user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"));
    return user.isEmpty() ? muse::qtrc("analysis", "Me") : user;
}

QString AnalysisService::authorId() const
{
    // Chosen once: the same person keeps the same layer file. From the Google account when known, so that one's
    // devices share one layer
    QSettings settings;
    QString id = settings.value(AUTHOR_ID_KEY).toString();
    if (id.isEmpty()) {
        const QString email = driveService() ? driveService()->accountEmail().toLower() : QString();
        const QByteArray seed = email.isEmpty() ? QUuid::createUuid().toByteArray() : email.toUtf8();
        id = QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha1).toHex().left(8));
        settings.setValue(AUTHOR_ID_KEY, id);
    }
    return id;
}

void AnalysisService::onProjectChanged()
{
    if (m_saveTimer.isActive()) {
        m_saveTimer.stop();
        saveMyLayer();
    }

    project::INotationProjectPtr project = globalContext()->currentProject();
    const QString path = project && !project->isNewlyCreated() ? project->path().toQString() : QString();

    m_active = false;
    m_drawing = false;
    m_moving = false;
    m_selectedId.clear();
    m_undo.clear();
    m_redo.clear();
    m_scorePath = path.endsWith(QStringLiteral(".mscz"), Qt::CaseInsensitive) ? path : QString();

    if (project) {
        // "Save as": the layers follow the new name from now on
        project->saveComplited().onReceive(this, [this](const io::path_t& newPath, project::SaveMode) {
            const QString p = newPath.toQString();
            if (p != m_scorePath && p.endsWith(QStringLiteral(".mscz"), Qt::CaseInsensitive)) {
                m_scorePath = p;
                for (Layer& layer : m_layers) {
                    if (layer.mine) {
                        layer.path = AnalysisLayerFile::pathFor(m_scorePath, layer.id);
                    }
                }
                saveMyLayer();
            }
        });
    }

    loadLayers();
    m_stateChanged.notify();
    m_repaintRequested.notify();
}

void AnalysisService::loadLayers()
{
    if (!m_watcher.files().isEmpty()) {
        m_watcher.removePaths(m_watcher.files());
    }
    if (!m_watcher.directories().isEmpty()) {
        m_watcher.removePaths(m_watcher.directories());
    }
    m_layers.clear();
    if (m_scorePath.isEmpty()) {
        return;
    }

    const QStringList hidden = QSettings().value(HIDDEN_LAYERS_KEY).toStringList();
    const QString myId = authorId();
    for (const QString& path : AnalysisLayerFile::pathsFor(m_scorePath)) {
        Layer layer;
        layer.path = path;
        QString error;
        if (!AnalysisLayerFile::load(path, layer.file, &error)) {
            LOGW() << "Analysis layer not read: " << path.toStdString() << ": " << error.toStdString();
            continue;
        }
        const QString name = QFileInfo(path).completeBaseName();
        layer.id = name.mid(name.lastIndexOf('.') + 1);
        layer.mine = layer.id == myId;
        layer.visible = layer.mine || !hidden.contains(layer.id);
        m_layers.push_back(layer);
        m_watcher.addPath(path);
    }
    m_watcher.addPath(QFileInfo(m_scorePath).absolutePath());

    // Mine first
    std::stable_sort(m_layers.begin(), m_layers.end(), [](const Layer& a, const Layer& b) { return a.mine && !b.mine; });
}

void AnalysisService::reloadChangedLayers()
{
    if (m_scorePath.isEmpty() || m_saving) {
        return;
    }

    const QStringList hidden = QSettings().value(HIDDEN_LAYERS_KEY).toStringList();
    const QString myId = authorId();
    bool any = false;
    for (const QString& path : AnalysisLayerFile::pathsFor(m_scorePath)) {
        const QString name = QFileInfo(path).completeBaseName();
        const QString id = name.mid(name.lastIndexOf('.') + 1);
        auto it = std::find_if(m_layers.begin(), m_layers.end(), [&](const Layer& l) { return l.id == id; });

        AnalysisLayerFile file;
        if (!AnalysisLayerFile::load(path, file)) {
            continue;
        }
        if (it == m_layers.end()) {
            Layer layer;
            layer.id = id;
            layer.path = path;
            layer.file = file;
            layer.mine = id == myId;
            layer.visible = layer.mine || !hidden.contains(id);
            m_layers.push_back(layer);
            any = true;
        } else if (file.modified > it->file.modified && !(it->mine && m_saveTimer.isActive())) {
            // Newer: from the sync, or from another window (for mine)
            it->file = file;
            any = true;
        }
        if (!m_watcher.files().contains(path)) {
            m_watcher.addPath(path);
        }
    }

    if (any) {
        std::stable_sort(m_layers.begin(), m_layers.end(), [](const Layer& a, const Layer& b) { return a.mine && !b.mine; });
        m_stateChanged.notify();
        m_repaintRequested.notify();
    }
}

AnalysisService::Layer* AnalysisService::myLayer()
{
    for (Layer& layer : m_layers) {
        if (layer.mine) {
            return &layer;
        }
    }
    return nullptr;
}

const AnalysisService::Layer* AnalysisService::myLayer() const
{
    for (const Layer& layer : m_layers) {
        if (layer.mine) {
            return &layer;
        }
    }
    return nullptr;
}

AnalysisService::Layer& AnalysisService::ensureMyLayer()
{
    if (Layer* mine = myLayer()) {
        return *mine;
    }
    Layer layer;
    layer.id = authorId();
    layer.path = AnalysisLayerFile::pathFor(m_scorePath, layer.id);
    layer.mine = true;
    layer.file.authorId = layer.id;
    layer.file.authorName = authorName();
    m_layers.insert(m_layers.begin(), layer);
    return m_layers.front();
}

QList<IAnalysisService::LayerInfo> AnalysisService::layers() const
{
    QList<LayerInfo> result;
    for (const Layer& layer : m_layers) {
        LayerInfo info;
        info.id = layer.id;
        info.author = layer.file.authorName.isEmpty() ? layer.id : layer.file.authorName;
        info.mine = layer.mine;
        info.visible = layer.visible;
        info.objectCount = int(layer.file.objects.size());
        result << info;
    }
    return result;
}

void AnalysisService::setLayerVisible(const QString& layerId, bool visible)
{
    QSettings settings;
    QStringList hidden = settings.value(HIDDEN_LAYERS_KEY).toStringList();
    hidden.removeAll(layerId);
    if (!visible) {
        hidden << layerId;
    }
    settings.setValue(HIDDEN_LAYERS_KEY, hidden);

    for (Layer& layer : m_layers) {
        if (layer.id == layerId) {
            layer.visible = visible;
        }
    }
    m_stateChanged.notify();
    m_repaintRequested.notify();
}

void AnalysisService::scheduleSave()
{
    m_saveTimer.start();
}

void AnalysisService::saveMyLayer()
{
    Layer* mine = myLayer();
    if (!mine || m_scorePath.isEmpty()) {
        return;
    }

    mine->file.authorId = mine->id;
    mine->file.authorName = authorName();
    mine->file.modified = QDateTime::currentDateTimeUtc();

    m_saving = true;
    QString error;
    if (mine->file.objects.empty()) {
        // Nothing left: no file (and the empty layer goes from Drive with the next sync)
        QFile::remove(mine->path);
    } else if (!mine->file.save(mine->path, &error)) {
        LOGE() << "Analysis layer not saved: " << error.toStdString();
    }
    m_saving = false;
    if (!m_watcher.files().contains(mine->path) && QFileInfo::exists(mine->path)) {
        m_watcher.addPath(mine->path);
    }

    // Shared with the score's Drive folder
    if (driveService() && driveService()->isAvailable()) {
        const QString root = QDir::cleanPath(driveService()->rootPath().toQString()) + "/";
        if (QDir::cleanPath(mine->path).startsWith(root)) {
            QTimer::singleShot(SYNC_AFTER_SAVE_MS, this, [this]() {
                if (driveService()) {
                    driveService()->sync();
                }
            });
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Anchors

bool AnalysisService::anchorAt(const PointF& canvasPos, AnalysisObject& obj, QPointF& origin) const
{
    const engraving::Score* s = score();
    if (!s) {
        return false;
    }

    engraving::Measure* measure = s->searchMeasure(canvasPos);
    if (!measure) {
        // Off the staves (a margin): the nearest measure
        double best = std::numeric_limits<double>::max();
        for (engraving::Measure* m = s->firstMeasure(); m; m = m->nextMeasure()) {
            if (!m->system()) {
                continue;
            }
            const RectF r = m->canvasBoundingRect();
            const double dx = std::max({ r.left() - canvasPos.x(), 0.0, canvasPos.x() - r.right() });
            const double dy = std::max({ r.top() - canvasPos.y(), 0.0, canvasPos.y() - r.bottom() });
            const double d = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                measure = m;
            }
        }
    }
    if (!measure || !measure->system()) {
        return false;
    }

    const engraving::System* system = measure->system();
    engraving::staff_idx_t staff = system->searchStaff(canvasPos.y() - system->canvasPos().y());
    if (staff == muse::nidx || staff >= s->nstaves()) {
        staff = 0;
    }

    obj.tickNum = measure->tick().numerator();
    obj.tickDen = measure->tick().denominator();
    obj.staff = int(staff);

    const std::optional<QPointF> o = originOf(obj);
    if (!o) {
        return false;
    }
    origin = *o;
    return true;
}

std::optional<QPointF> AnalysisService::originOf(const AnalysisObject& obj) const
{
    const engraving::Score* s = score();
    if (!s) {
        return std::nullopt;
    }

    engraving::Measure* measure = s->tick2measure(engraving::Fraction(obj.tickNum, obj.tickDen));
    if (!measure || !measure->system()) {
        return std::nullopt; // the measure is gone: the object is not shown (and is kept)
    }

    const engraving::System* system = measure->system();
    size_t staff = obj.staff >= 0 && size_t(obj.staff) < s->nstaves() ? size_t(obj.staff) : 0;
    engraving::SysStaff* sysStaff = system->staff(staff);
    if (!sysStaff || !sysStaff->show()) {
        sysStaff = system->staff(0);
    }

    const PointF measurePos = measure->canvasPos();
    const double y = system->canvasPos().y() + (sysStaff ? sysStaff->y() : 0.0);
    return QPointF(measurePos.x(), y);
}

std::vector<QPointF> AnalysisService::canvasPoints(const AnalysisObject& obj) const
{
    std::vector<QPointF> result;
    const std::optional<QPointF> origin = originOf(obj);
    if (!origin) {
        return result;
    }
    const double sp = spatium();
    result.reserve(obj.points.size());
    for (const QPointF& p : obj.points) {
        result.push_back(*origin + p * sp);
    }
    return result;
}

QRectF AnalysisService::canvasBounds(const AnalysisObject& obj) const
{
    const std::vector<QPointF> points = canvasPoints(obj);
    if (points.empty()) {
        return QRectF();
    }
    const double sp = spatium();

    if (obj.shape == AnalysisObject::Shape::Text || obj.shape == AnalysisObject::Shape::Symbol) {
        const double h = obj.textSize * sp;
        const double w = std::max(1, int(obj.text.size())) * h * 0.6;
        return QRectF(points.front(), QSizeF(w, h));
    }

    QRectF r(points.front(), QSizeF(0, 0));
    for (const QPointF& p : points) {
        r = r.united(QRectF(p, QSizeF(0, 0)));
    }
    return r.normalized();
}

QString AnalysisService::hitTest(const PointF& pos) const
{
    const Layer* mine = myLayer();
    if (!mine) {
        return QString();
    }
    const double tolerance = HIT_TOLERANCE_SP * spatium();
    for (auto it = mine->file.objects.rbegin(); it != mine->file.objects.rend(); ++it) {
        const QRectF r = canvasBounds(*it).adjusted(-tolerance, -tolerance, tolerance, tolerance);
        if (r.contains(toQ(pos))) {
            return it->id;
        }
    }
    return QString();
}

// ---------------------------------------------------------------------------------------------------------------------
// Editing

QString AnalysisService::newId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(13);
}

AnalysisObject::Shape AnalysisService::shapeForTool() const
{
    switch (m_tool) {
    case Tool::Line: return AnalysisObject::Shape::Line;
    case Tool::Arrow: return AnalysisObject::Shape::Arrow;
    case Tool::Circle: return AnalysisObject::Shape::Circle;
    case Tool::Text: return AnalysisObject::Shape::Text;
    case Tool::Symbol: return AnalysisObject::Shape::Symbol;
    default: return AnalysisObject::Shape::Pen;
    }
}

void AnalysisService::pushUndo()
{
    m_undo.push_back(ensureMyLayer().file.objects);
    m_redo.clear();
}

void AnalysisService::changed()
{
    scheduleSave();
    m_stateChanged.notify();
    m_repaintRequested.notify();
}

bool AnalysisService::canUndo() const
{
    return !m_undo.empty();
}

bool AnalysisService::canRedo() const
{
    return !m_redo.empty();
}

void AnalysisService::undo()
{
    if (m_undo.empty()) {
        return;
    }
    Layer& mine = ensureMyLayer();
    m_redo.push_back(mine.file.objects);
    mine.file.objects = m_undo.back();
    m_undo.pop_back();
    m_selectedId.clear();
    changed();
}

void AnalysisService::redo()
{
    if (m_redo.empty()) {
        return;
    }
    Layer& mine = ensureMyLayer();
    m_undo.push_back(mine.file.objects);
    mine.file.objects = m_redo.back();
    m_redo.pop_back();
    m_selectedId.clear();
    changed();
}

bool AnalysisService::hasSelection() const
{
    return !m_selectedId.isEmpty();
}

void AnalysisService::deleteSelection()
{
    Layer* mine = myLayer();
    if (!mine || m_selectedId.isEmpty()) {
        return;
    }
    pushUndo();
    auto& objects = mine->file.objects;
    objects.erase(std::remove_if(objects.begin(), objects.end(), [this](const AnalysisObject& o) { return o.id == m_selectedId; }),
                  objects.end());
    m_selectedId.clear();
    changed();
}

void AnalysisService::clearMyLayer()
{
    Layer* mine = myLayer();
    if (!mine || mine->file.objects.empty()) {
        return;
    }
    pushUndo();
    mine->file.objects.clear();
    m_selectedId.clear();
    changed();
}

async::Notification AnalysisService::textRequested() const
{
    return m_textRequested;
}

void AnalysisService::addText(const QString& text)
{
    const QString clean = text.trimmed();
    if (clean.isEmpty() || !isActive()) {
        return;
    }
    AnalysisObject obj;
    QPointF origin;
    if (!anchorAt(m_textPos, obj, origin)) {
        return;
    }
    obj.id = newId();
    obj.shape = AnalysisObject::Shape::Text;
    obj.color = m_color;
    obj.text = clean;
    obj.textSize = TEXT_SIZE_SP;
    // The tap is the middle of the first line
    obj.points.push_back((toQ(m_textPos) - origin) / spatium() - QPointF(0, TEXT_SIZE_SP / 2));

    pushUndo();
    ensureMyLayer().file.objects.push_back(obj);
    changed();
}

bool AnalysisService::pointerPress(const PointF& pos)
{
    if (!isActive()) {
        return false;
    }

    switch (m_tool) {
    case Tool::Select: {
        m_selectedId = hitTest(pos);
        if (!m_selectedId.isEmpty()) {
            const Layer* mine = myLayer();
            for (const AnalysisObject& o : mine->file.objects) {
                if (o.id == m_selectedId) {
                    m_moveOriginalPoints = o.points;
                }
            }
            m_moving = true;
            m_moveStart = toQ(pos);
        }
        m_stateChanged.notify();
        m_repaintRequested.notify();
        return true;
    }
    case Tool::Eraser: {
        m_selectedId = hitTest(pos);
        deleteSelection();
        return true;
    }
    case Tool::Text:
    case Tool::Symbol:
        return true; // on release
    case Tool::Pen:
    case Tool::Line:
    case Tool::Arrow:
    case Tool::Circle: {
        m_draft = AnalysisObject();
        if (!anchorAt(pos, m_draft, m_draftOrigin)) {
            return true;
        }
        m_draft.id = newId();
        m_draft.shape = shapeForTool();
        m_draft.color = m_color;
        m_draft.width = m_width;
        const QPointF p = (toQ(pos) - m_draftOrigin) / spatium();
        m_draft.points = { p, p };
        if (m_draft.shape == AnalysisObject::Shape::Pen) {
            m_draft.points.pop_back();
        }
        m_drawing = true;
        return true;
    }
    }
    return true;
}

bool AnalysisService::pointerMove(const PointF& pos)
{
    if (!isActive()) {
        return false;
    }

    const double sp = spatium();
    if (m_drawing) {
        const QPointF p = (toQ(pos) - m_draftOrigin) / sp;
        if (m_draft.shape == AnalysisObject::Shape::Pen) {
            if (QLineF(m_draft.points.back(), p).length() >= PEN_MIN_STEP_SP) {
                m_draft.points.push_back(p);
            }
        } else {
            m_draft.points.back() = p;
        }
        m_repaintRequested.notify();
        return true;
    }

    if (m_moving) {
        Layer* mine = myLayer();
        const QPointF delta = (toQ(pos) - m_moveStart) / sp;
        for (AnalysisObject& o : mine->file.objects) {
            if (o.id == m_selectedId) {
                for (size_t i = 0; i < o.points.size() && i < m_moveOriginalPoints.size(); ++i) {
                    o.points[i] = m_moveOriginalPoints[i] + delta;
                }
            }
        }
        m_repaintRequested.notify();
        return true;
    }

    return true;
}

bool AnalysisService::pointerRelease(const PointF& pos)
{
    if (!isActive()) {
        return false;
    }

    const double sp = spatium();

    if (m_drawing) {
        m_drawing = false;
        bool keep = false;
        if (m_draft.shape == AnalysisObject::Shape::Pen) {
            keep = m_draft.points.size() >= 2;
        } else {
            keep = QLineF(m_draft.points.front(), m_draft.points.back()).length() >= MIN_SHAPE_SP;
        }
        if (keep) {
            pushUndo();
            ensureMyLayer().file.objects.push_back(m_draft);
            changed();
        } else {
            m_repaintRequested.notify();
        }
        return true;
    }

    if (m_moving) {
        m_moving = false;
        const QPointF delta = (toQ(pos) - m_moveStart) / sp;
        if (QLineF(QPointF(), delta).length() > 0.05) {
            // The undo step is the state before the move
            Layer* mine = myLayer();
            std::vector<AnalysisObject> before = mine->file.objects;
            for (AnalysisObject& o : before) {
                if (o.id == m_selectedId) {
                    o.points = m_moveOriginalPoints;
                }
            }
            m_undo.push_back(before);
            m_redo.clear();
            changed();
        }
        return true;
    }

    if (m_tool == Tool::Text) {
        m_textPos = pos;
        m_textRequested.notify();
        return true;
    }

    if (m_tool == Tool::Symbol && !m_symbol.isEmpty()) {
        AnalysisObject obj;
        QPointF origin;
        if (anchorAt(pos, obj, origin)) {
            obj.id = newId();
            obj.shape = AnalysisObject::Shape::Symbol;
            obj.color = m_color;
            obj.text = m_symbol;
            obj.textSize = SYMBOL_SIZE_SP;
            // Centred on the tap
            obj.points.push_back((toQ(pos) - origin) / sp - QPointF(SYMBOL_SIZE_SP * 0.3, SYMBOL_SIZE_SP / 2));
            pushUndo();
            ensureMyLayer().file.objects.push_back(obj);
            changed();
        }
        return true;
    }

    return true;
}

bool AnalysisService::keyPress(int key, Qt::KeyboardModifiers modifiers)
{
    if (!isActive()) {
        return false;
    }
    if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
        deleteSelection();
        return true;
    }
    if (key == Qt::Key_Escape) {
        m_drawing = false;
        m_moving = false;
        m_selectedId.clear();
        m_stateChanged.notify();
        m_repaintRequested.notify();
        return true;
    }
    if (key == Qt::Key_Z && (modifiers & Qt::ControlModifier)) {
        (modifiers & Qt::ShiftModifier) ? redo() : undo();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting

void AnalysisService::paintObject(draw::Painter* painter, const AnalysisObject& obj, bool selected) const
{
    const std::vector<QPointF> qpoints = canvasPoints(obj);
    if (qpoints.empty()) {
        return;
    }
    std::vector<PointF> points;
    points.reserve(qpoints.size());
    for (const QPointF& p : qpoints) {
        points.push_back(PointF::fromQPointF(p));
    }

    const double sp = spatium();
    const Color color = Color::fromQColor(obj.color);
    draw::Pen pen(color, std::max(0.5, obj.width * sp), draw::PenStyle::SolidLine, draw::PenCapStyle::RoundCap,
                  draw::PenJoinStyle::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(draw::BrushStyle::NoBrush);

    switch (obj.shape) {
    case AnalysisObject::Shape::Pen:
        painter->drawPolyline(points.data(), points.size());
        break;
    case AnalysisObject::Shape::Line:
    case AnalysisObject::Shape::Arrow: {
        const PointF a = points.front();
        const PointF b = points.back();
        painter->drawPolyline(points.data(), 2);
        if (obj.shape == AnalysisObject::Shape::Arrow) {
            const double angle = std::atan2(b.y() - a.y(), b.x() - a.x());
            const double head = std::max(0.8 * sp, obj.width * sp * 4);
            for (double side : { 0.45, -0.45 }) {
                const PointF wing(b.x() - head * std::cos(angle + side), b.y() - head * std::sin(angle + side));
                const PointF line[2] = { b, wing };
                painter->drawPolyline(line, 2);
            }
        }
        break;
    }
    case AnalysisObject::Shape::Circle: {
        const RectF r = RectF(points.front(), points.back()).normalized();
        painter->drawEllipse(r);
        break;
    }
    case AnalysisObject::Shape::Text:
    case AnalysisObject::Shape::Symbol: {
        draw::Font font(draw::Font::FontFamily(u"Edwin"), draw::Font::Type::Text);
        const double heightCanvas = obj.textSize * sp;
        // The painter scales point sizes from the engraving's 1200 DPI
        font.setPointSizeF(heightCanvas * 72.0 / 1200.0);
        painter->setFont(font);
        const PointF baseline(points.front().x(), points.front().y() + heightCanvas * 0.8);
        painter->drawText(baseline, String::fromQString(obj.text));
        break;
    }
    }

    if (selected) {
        const QRectF b = canvasBounds(obj).adjusted(-0.3 * sp, -0.3 * sp, 0.3 * sp, 0.3 * sp);
        draw::Pen dashed(Color(30, 120, 255), std::max(0.5, 0.08 * sp), draw::PenStyle::DashLine);
        painter->setPen(dashed);
        painter->setBrush(draw::BrushStyle::NoBrush);
        painter->drawRoundedRect(RectF::fromQRectF(b), 0.2 * sp, 0.2 * sp);
    }
}

void AnalysisService::paintLayers(draw::Painter* painter, const QRectF& clip) const
{
    painter->save();
    painter->setAntialiasing(true);
    // Others below, mine on top
    for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
        if (!it->visible) {
            continue;
        }
        for (const AnalysisObject& obj : it->file.objects) {
            if (clip.isValid() && !canvasBounds(obj).intersects(clip)) {
                continue;
            }
            paintObject(painter, obj, it->mine && obj.id == m_selectedId);
        }
    }
    painter->restore();
}

void AnalysisService::paint(draw::Painter* painter)
{
    if (!isActive()) {
        return;
    }
    paintLayers(painter);
    if (m_drawing) {
        painter->save();
        painter->setAntialiasing(true);
        paintObject(painter, m_draft, false);
        painter->restore();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// PDF

Ret AnalysisService::exportPdf()
{
    notation::INotationPtr notation = globalContext()->currentNotation();
    if (!notation || !isActive()) {
        return make_ret(Ret::Code::UnknownError);
    }

    const QFileInfo score(m_scorePath);
    //: The name of the exported PDF: %1 is the name of the score
    const QString fileName = muse::qtrc("analysis", "%1 (analysis).pdf").arg(score.completeBaseName());

#ifdef Q_OS_ANDROID
    const QString dir = globalConfiguration()->userAppDataPath().toQString() + "/exports";
    QDir().mkpath(dir);
    const QString path = dir + "/" + fileName;
#else
    const QString path = interactive()->selectSavingFileSync(muse::trc("analysis", "Export with analysis"),
                                                              io::path_t(score.absolutePath() + "/" + fileName),
                                                              { muse::trc("analysis", "PDF file") + " (*.pdf)" }).toQString();
    if (path.isEmpty()) {
        return make_ret(Ret::Code::Cancel);
    }
#endif

    const SizeF pageSize = notation->painting()->pageSizeInch();
    QPdfWriter pdf(path);
    pdf.setPageSize(QPageSize(QSizeF(pageSize.width(), pageSize.height()), QPageSize::Inch));
    pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
    pdf.setTitle(score.completeBaseName());
    pdf.setCreator(QStringLiteral("VibeScore"));
    pdf.setResolution(300);

    draw::Painter painter(&pdf, "analysispdf");
    if (!painter.isActive()) {
        return make_ret(Ret::Code::UnknownError, muse::trc("analysis", "The PDF could not be written."));
    }

    const auto& pages = notation->elements()->pages();
    for (size_t i = 0; i < pages.size(); ++i) {
        if (i > 0) {
            pdf.newPage();
        }
        notation::INotationPainting::Options opt;
        opt.deviceDpi = pdf.logicalDpiX();
        opt.fromPage = int(i);
        opt.toPage = int(i);
        opt.printPageBackground = true;
        notation->painting()->paintPdf(&painter, opt);

        // The page is drawn in its own coordinates: the analysis too
        const engraving::Page* page = pages.at(i);
        const QRectF pageRect = QRectF(toQ(page->pos()), page->ldata()->bbox().size().toQSizeF());
        painter.save();
        painter.translate(-page->pos().x(), -page->pos().y());
        paintLayers(&painter, pageRect);
        painter.restore();
    }
    painter.endDraw();

#ifdef Q_OS_ANDROID
    QJniEnvironment env;
    jobjectArray jpaths = env->NewObjectArray(1, env.findClass("java/lang/String"), nullptr);
    const QJniObject jpath = QJniObject::fromString(path);
    env->SetObjectArrayElement(jpaths, 0, jpath.object<jstring>());
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject title = QJniObject::fromString(muse::qtrc("analysis", "Share the score with analysis"));
    QJniObject::callStaticMethod<void>("io/github/m1omg/vibescore/VibeScoreShare", "shareFiles",
                                       "(Landroid/content/Context;[Ljava/lang/String;Ljava/lang/String;)V",
                                       context.object(), jpaths, title.object<jstring>());
    env.checkAndClearExceptions();
#else
    interactive()->revealInFileBrowser(io::path_t(path));
#endif
    return make_ok();
}
