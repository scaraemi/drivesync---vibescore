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
#include "notationviewtouchcontroller.h"

#include <cmath>

#include <QGuiApplication>
#include <QHoverEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QQuickItem>
#include <QStyleHints>
#include <QTouchEvent>

#include "notationviewinputcontroller.h"

#include "muse_framework_config.h"

using namespace mu::notation;

static constexpr qreal DOUBLE_TAP_DISTANCE = 24;   // view pixels (dp)
static constexpr int FLING_INTERVAL_MS = 16;
static constexpr qreal FLING_MIN_SPEED = 0.3;      // pixels per millisecond
static constexpr qreal FLING_STOP_SPEED = 0.02;
static constexpr qreal FLING_DECAY_PER_MS = 0.996;

NotationViewTouchController::NotationViewTouchController(IControlledView* view, NotationViewInputController* inputController,
                                                         QObject* parent)
    : QObject(parent), m_view(view), m_input(inputController)
{
    m_longPressTimer.setSingleShot(true);
    m_longPressTimer.setInterval(LONG_PRESS_INTERVAL_MS);
    connect(&m_longPressTimer, &QTimer::timeout, this, &NotationViewTouchController::onLongPressTimeout);

    m_flingTimer.setInterval(FLING_INTERVAL_MS);
    connect(&m_flingTimer, &QTimer::timeout, this, &NotationViewTouchController::onFlingStep);
}

bool NotationViewTouchController::isEnabled()
{
    static const bool enabled = []() {
        if (qEnvironmentVariableIsSet("MUSE_UI_TOUCH")) {
            return qEnvironmentVariableIntValue("MUSE_UI_TOUCH") != 0;
        }
#ifdef MUSE_CONFIGURATION_IS_MOBILE
        return true;
#else
        return false;
#endif
    }();

    return enabled;
}

bool NotationViewTouchController::handleTouchEvent(QTouchEvent* event)
{
    // A stylus or a touchpad keeps the default (mouse) handling
    if (event->device() && event->device()->type() != QInputDevice::DeviceType::TouchScreen) {
        return false;
    }

    TouchPoints pressedPoints;
    for (const QEventPoint& point : event->points()) {
        if (point.state() != QEventPoint::Released) {
            pressedPoints.push_back({ point.id(), point.position() });
        }
    }

    switch (event->type()) {
    case QEvent::TouchBegin:
        touchBegin(pressedPoints);
        return true;
    case QEvent::TouchUpdate:
        touchUpdate(pressedPoints);
        return true;
    case QEvent::TouchEnd:
        touchEnd(event->points().isEmpty() ? m_lastPos : event->points().first().position());
        return true;
    case QEvent::TouchCancel:
        cancel();
        return true;
    default:
        break;
    }

    return false;
}

void NotationViewTouchController::touchBegin(const TouchPoints& points)
{
    stopFling();

    if (points.empty()) {
        return;
    }

    m_primaryPointId = points.front().id;
    m_startPos = m_lastPos = points.front().pos;
    m_velocity = QPointF();
    m_moveTimer.start();

    emit focusRequested();

    if (points.size() >= 2) {
        beginPinch(points.at(0).pos, points.at(1).pos);
    } else {
        m_state = State::Pending;
        m_longPressTimer.start();
    }

    updateMovingByGesture();
}

void NotationViewTouchController::touchUpdate(const TouchPoints& points)
{
    if (points.size() >= 2) {
        if (m_state == State::Pinching) {
            updatePinch(points.at(0).pos, points.at(1).pos);
            return;
        }

        m_longPressTimer.stop();

        // A second finger ends what the first one was doing; it never moves notes
        if (m_state == State::Dragging) {
            sendMouse(QEvent::MouseButtonRelease, m_lastPos, Qt::LeftButton, Qt::NoButton);
        } else if (m_state == State::NotePreview) {
            sendHoverLeave(m_lastPos);
        }

        beginPinch(points.at(0).pos, points.at(1).pos);
        return;
    }

    if (points.empty()) {
        return;
    }

    // Follow the first finger; after a pinch, the remaining finger pans
    const TouchPoint* point = &points.front();
    for (const TouchPoint& p : points) {
        if (p.id == m_primaryPointId) {
            point = &p;
            break;
        }
    }

    const QPointF pos = point->pos;

    switch (m_state) {
    case State::Pending:
        if (QLineF(pos, m_startPos).length() > TAP_SLOP) {
            m_longPressTimer.stop();

            // Shift+drag selects a range (or what a lasso surrounds)
            const bool isDragTarget = (modifiers() & Qt::ShiftModifier) || m_input->isTouchDragTarget(m_startPos);
            if (!m_view->isNoteEnterMode() && isDragTarget) {
                m_state = State::Dragging;
                sendMouse(QEvent::MouseButtonPress, m_startPos, Qt::LeftButton, Qt::LeftButton);
                sendMouse(QEvent::MouseMove, pos, Qt::NoButton, Qt::LeftButton);
            } else {
                m_state = State::Panning;
                updateMovingByGesture();
                pan(pos - m_lastPos);
            }
        }
        break;
    case State::Pinching:
        m_primaryPointId = point->id;
        m_state = State::Panning;
        break;
    case State::Panning:
        pan(pos - m_lastPos);
        break;
    case State::Dragging:
        sendMouse(QEvent::MouseMove, pos, Qt::NoButton, Qt::LeftButton);
        break;
    case State::NotePreview:
        sendHover(pos, m_lastPos);
        break;
    case State::LongPressed:
    case State::Idle:
        break;
    }

    m_lastPos = pos;
}

void NotationViewTouchController::touchEnd(const QPointF& lastPos)
{
    m_longPressTimer.stop();

    switch (m_state) {
    case State::Pending:
        onTap(lastPos);
        break;
    case State::Panning:
        startFling();
        break;
    case State::Dragging:
        sendMouse(QEvent::MouseButtonRelease, lastPos, Qt::LeftButton, Qt::NoButton);
        break;
    case State::NotePreview:
        // Put the note where the preview is
        sendMouse(QEvent::MouseButtonPress, lastPos, Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, lastPos, Qt::LeftButton, Qt::NoButton);
        break;
    case State::LongPressed:
    case State::Pinching:
    case State::Idle:
        break;
    }

    m_state = State::Idle;
    updateMovingByGesture(); // still moving while the score is flung
}

void NotationViewTouchController::cancel()
{
    m_longPressTimer.stop();

    if (m_state == State::Dragging) {
        sendMouse(QEvent::MouseButtonRelease, m_lastPos, Qt::LeftButton, Qt::NoButton);
    } else if (m_state == State::NotePreview) {
        sendHoverLeave(m_lastPos);
    }

    m_state = State::Idle;
    stopFling();
}

void NotationViewTouchController::onTap(const QPointF& pos)
{
    const bool isDoubleTap = m_hasLastTap
                             && m_lastTapTimer.elapsed() <= QGuiApplication::styleHints()->mouseDoubleClickInterval()
                             && QLineF(pos, m_lastTapPos).length() <= DOUBLE_TAP_DISTANCE;

    // Same order as Qt Quick delivers a mouse double click: press, double click, release
    sendMouse(QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);

    if (isDoubleTap) {
        sendMouse(QEvent::MouseButtonDblClick, pos, Qt::LeftButton, Qt::LeftButton);
        m_hasLastTap = false;
    } else {
        m_hasLastTap = true;
        m_lastTapPos = pos;
        m_lastTapTimer.start();
    }

    sendMouse(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

void NotationViewTouchController::onLongPressTimeout()
{
    if (m_input->isAnalysisActive()) {
        return; // no context menu while drawing on the score
    }

    if (m_state != State::Pending) {
        return;
    }

    // The finger feels that the long press was recognized
    if (hapticFeedback()) {
        hapticFeedback()->perform(muse::ui::IHapticFeedback::Type::LongPress);
    }

    if (m_view->isNoteEnterMode()) {
        m_state = State::NotePreview;

        // The hover handler ignores events that do not move, so the "old" position must differ
        sendHover(m_startPos, m_startPos + QPointF(TAP_SLOP, TAP_SLOP));
        return;
    }

    m_state = State::LongPressed;

    // Select what is under the finger, like a right click does, then show the context menu
    sendMouse(QEvent::MouseButtonPress, m_startPos, Qt::RightButton, Qt::RightButton);
    sendMouse(QEvent::MouseButtonRelease, m_startPos, Qt::RightButton, Qt::NoButton);
    m_view->showContextMenu(m_input->selectionType(), m_startPos);
}

void NotationViewTouchController::pan(const QPointF& delta)
{
    const qreal scaling = m_view->currentScaling();
    if (qFuzzyIsNull(scaling)) {
        return;
    }

    m_view->moveCanvas(delta.x() / scaling, delta.y() / scaling);

    const qint64 elapsed = m_moveTimer.restart();
    if (elapsed > 0) {
        const QPointF velocity = delta / static_cast<qreal>(elapsed);
        m_velocity = 0.8 * velocity + 0.2 * m_velocity;
    }
}

void NotationViewTouchController::beginPinch(const QPointF& p1, const QPointF& p2)
{
    m_state = State::Pinching;
    updateMovingByGesture();
    m_lastPinchDistance = QLineF(p1, p2).length();
    m_lastPinchCenter = (p1 + p2) / 2;
}

void NotationViewTouchController::updatePinch(const QPointF& p1, const QPointF& p2)
{
    const qreal distance = QLineF(p1, p2).length();
    const QPointF center = (p1 + p2) / 2;

    if (m_lastPinchDistance > 0 && distance > 0 && !qFuzzyCompare(distance, m_lastPinchDistance)) {
        m_input->pinchToZoom(distance / m_lastPinchDistance, center);
    }

    const QPointF delta = center - m_lastPinchCenter;
    const qreal scaling = m_view->currentScaling();
    if (!delta.isNull() && !qFuzzyIsNull(scaling)) {
        m_view->moveCanvas(delta.x() / scaling, delta.y() / scaling);
    }

    m_lastPinchDistance = distance;
    m_lastPinchCenter = center;
}

void NotationViewTouchController::startFling()
{
    // A finger that rested before lifting does not throw the score
    if (m_moveTimer.elapsed() > 100) {
        return;
    }

    const qreal speed = QLineF(QPointF(), m_velocity).length();
    if (speed < FLING_MIN_SPEED) {
        return;
    }

    m_flingClock.start();
    m_flingTimer.start();
}

void NotationViewTouchController::onFlingStep()
{
    const qint64 elapsed = std::max<qint64>(m_flingClock.restart(), 1);

    m_velocity *= std::pow(FLING_DECAY_PER_MS, static_cast<qreal>(elapsed));
    if (QLineF(QPointF(), m_velocity).length() < FLING_STOP_SPEED) {
        stopFling();
        return;
    }

    const qreal scaling = m_view->currentScaling();
    if (qFuzzyIsNull(scaling)) {
        stopFling();
        return;
    }

    const QPointF delta = m_velocity * static_cast<qreal>(elapsed);
    if (!m_view->moveCanvas(delta.x() / scaling, delta.y() / scaling)) {
        // Reached the edge of the score
        stopFling();
    }
}

void NotationViewTouchController::stopFling()
{
    m_flingTimer.stop();
    updateMovingByGesture();
}

void NotationViewTouchController::updateMovingByGesture()
{
    const bool moving = m_state == State::Panning || m_state == State::Pinching || m_flingTimer.isActive();
    m_view->setMovingByGesture(moving);
}

Qt::KeyboardModifiers NotationViewTouchController::modifiers() const
{
    return touchModifiers() ? touchModifiers()->modifiers() : Qt::NoModifier;
}

void NotationViewTouchController::sendMouse(QEvent::Type type, const QPointF& pos, Qt::MouseButton button,
                                            Qt::MouseButtons buttons)
{
    QQuickItem* item = m_view->asItem();
    const QPointF scenePos = item ? item->mapToScene(pos) : pos;
    const QPointF globalPos = item ? item->mapToGlobal(pos) : pos;
    QMouseEvent event(type, pos, scenePos, globalPos, button, buttons, modifiers());

    // Fingers are less precise than a mouse: hit testing is more tolerant for these events
    m_input->setTouchInput(true);

    switch (type) {
    case QEvent::MouseButtonPress:
        m_input->mousePressEvent(&event);
        break;
    case QEvent::MouseMove:
        m_input->mouseMoveEvent(&event);
        break;
    case QEvent::MouseButtonRelease:
        m_input->mouseReleaseEvent(&event);
        break;
    case QEvent::MouseButtonDblClick:
        m_input->mouseDoubleClickEvent(&event);
        break;
    default:
        break;
    }

    m_input->setTouchInput(false);
}

void NotationViewTouchController::sendHover(const QPointF& pos, const QPointF& oldPos)
{
    QQuickItem* item = m_view->asItem();
    QHoverEvent event(QEvent::HoverMove, pos, item ? item->mapToGlobal(pos) : pos, oldPos);
    m_input->hoverMoveEvent(&event);
}

void NotationViewTouchController::sendHoverLeave(const QPointF& pos)
{
    QQuickItem* item = m_view->asItem();
    QHoverEvent event(QEvent::HoverLeave, pos, item ? item->mapToGlobal(pos) : pos, pos);
    m_input->hoverLeaveEvent(&event);
}
