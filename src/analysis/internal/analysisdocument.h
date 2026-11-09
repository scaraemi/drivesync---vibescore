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

#include <vector>

#include <QColor>
#include <QDateTime>
#include <QPointF>
#include <QString>

namespace mu::analysis {
//! One object of an analysis layer. Positions are in staff spaces, relative to the anchor: the left edge of the
//! measure at the top line of the staff. The first point is where the object was started.
struct AnalysisObject {
    enum class Shape {
        Pen,
        Line,
        Arrow,
        Circle,
        Text,
        Symbol,
    };

    QString id;
    Shape shape = Shape::Pen;
    QColor color = Qt::red;
    double width = 0.25;          // staff spaces
    int tickNum = 0;              // the measure: its start tick, a fraction
    int tickDen = 1;
    int staff = 0;
    std::vector<QPointF> points;  // staff spaces from the anchor
    QString text;                 // Text, Symbol
    double textSize = 2.0;        // staff spaces (height)
};

//! One person's layer on one score: "<score>.<author id>.vsanalysis", JSON
struct AnalysisLayerFile {
    QString authorId;
    QString authorName;
    QDateTime modified;
    std::vector<AnalysisObject> objects;

    static bool load(const QString& path, AnalysisLayerFile& layer, QString* error = nullptr);
    bool save(const QString& path, QString* error = nullptr) const;

    //! "<dir>/<name>.mscz" -> "<dir>/<name>.<authorId>.vsanalysis"
    static QString pathFor(const QString& scorePath, const QString& authorId);
    //! The layers next to a score
    static QStringList pathsFor(const QString& scorePath);
};
}
