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
#include "analysisdocument.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

using namespace mu::analysis;

static constexpr int FORMAT_VERSION = 1;

static QString shapeName(AnalysisObject::Shape shape)
{
    switch (shape) {
    case AnalysisObject::Shape::Pen: return "pen";
    case AnalysisObject::Shape::Line: return "line";
    case AnalysisObject::Shape::Arrow: return "arrow";
    case AnalysisObject::Shape::Circle: return "circle";
    case AnalysisObject::Shape::Text: return "text";
    case AnalysisObject::Shape::Symbol: return "symbol";
    }
    return "pen";
}

static bool shapeFromName(const QString& name, AnalysisObject::Shape& shape)
{
    static const QList<QPair<QString, AnalysisObject::Shape> > shapes {
        { "pen", AnalysisObject::Shape::Pen }, { "line", AnalysisObject::Shape::Line },
        { "arrow", AnalysisObject::Shape::Arrow }, { "circle", AnalysisObject::Shape::Circle },
        { "text", AnalysisObject::Shape::Text }, { "symbol", AnalysisObject::Shape::Symbol },
    };
    for (const auto& [n, s] : shapes) {
        if (n == name) {
            shape = s;
            return true;
        }
    }
    return false; // a shape of a newer version: skipped
}

bool AnalysisLayerFile::load(const QString& path, AnalysisLayerFile& layer, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll(), &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = parseError.errorString();
        }
        return false;
    }

    layer.authorId = root.value("authorId").toString();
    layer.authorName = root.value("author").toString();
    layer.modified = QDateTime::fromString(root.value("modified").toString(), Qt::ISODate);
    layer.objects.clear();

    for (const QJsonValue& v : root.value("objects").toArray()) {
        const QJsonObject o = v.toObject();
        AnalysisObject obj;
        if (!shapeFromName(o.value("shape").toString(), obj.shape)) {
            continue;
        }
        obj.id = o.value("id").toString();
        obj.color = QColor(o.value("color").toString());
        obj.width = o.value("width").toDouble(0.25);
        obj.tickNum = o.value("tick").toArray().at(0).toInt();
        obj.tickDen = qMax(1, o.value("tick").toArray().at(1).toInt(1));
        obj.staff = o.value("staff").toInt();
        obj.text = o.value("text").toString();
        obj.textSize = o.value("textSize").toDouble(2.0);
        const QJsonArray points = o.value("points").toArray();
        for (int i = 0; i + 1 < points.size(); i += 2) {
            obj.points.emplace_back(points.at(i).toDouble(), points.at(i + 1).toDouble());
        }
        if (!obj.points.empty()) {
            layer.objects.push_back(obj);
        }
    }
    return true;
}

bool AnalysisLayerFile::save(const QString& path, QString* error) const
{
    QJsonArray objectsJson;
    for (const AnalysisObject& obj : objects) {
        QJsonArray points;
        for (const QPointF& p : obj.points) {
            // 3 decimals of a staff space: enough, and the files stay small
            points.append(qRound(p.x() * 1000) / 1000.0);
            points.append(qRound(p.y() * 1000) / 1000.0);
        }
        QJsonObject o {
            { "id", obj.id },
            { "shape", shapeName(obj.shape) },
            { "color", obj.color.name(QColor::HexArgb) },
            { "width", obj.width },
            { "tick", QJsonArray { obj.tickNum, obj.tickDen } },
            { "staff", obj.staff },
            { "points", points },
        };
        if (!obj.text.isEmpty()) {
            o.insert("text", obj.text);
            o.insert("textSize", obj.textSize);
        }
        objectsJson.append(o);
    }

    const QJsonObject root {
        { "format", "vibescore-analysis" },
        { "version", FORMAT_VERSION },
        { "authorId", authorId },
        { "author", authorName },
        { "modified", modified.toUTC().toString(Qt::ISODate) },
        { "objects", objectsJson },
    };

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

QString AnalysisLayerFile::pathFor(const QString& scorePath, const QString& authorId)
{
    const QFileInfo score(scorePath);
    return score.absolutePath() + "/" + score.completeBaseName() + "." + authorId + ".vsanalysis";
}

QStringList AnalysisLayerFile::pathsFor(const QString& scorePath)
{
    const QFileInfo score(scorePath);
    QStringList paths;
    // "<name>.<id>.vsanalysis": exactly one more dot than the score's name
    const QString prefix = score.completeBaseName() + ".";
    for (const QFileInfo& f : QDir(score.absolutePath()).entryInfoList({ prefix + "*.vsanalysis" }, QDir::Files)) {
        const QString middle = f.fileName().mid(prefix.size()).chopped(QStringLiteral(".vsanalysis").size());
        if (!middle.isEmpty() && !middle.contains('.')) {
            paths << f.absoluteFilePath();
        }
    }
    return paths;
}
