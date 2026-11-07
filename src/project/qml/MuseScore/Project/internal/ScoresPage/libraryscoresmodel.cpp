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
#include "libraryscoresmodel.h"

#include <algorithm>

#include <QDateTime>

#include "translation.h"
#include "dataformatter.h"
#include "io/fileinfo.h"

#include "engraving/infrastructure/mscio.h"

#include "log.h"

using namespace muse;
using namespace mu::project;

static const std::vector<std::string> SCORE_FILTERS { "*.mscz", "*.mscx" };

LibraryScoresModel::LibraryScoresModel(QObject* parent)
    : AbstractScoresModel(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

void LibraryScoresModel::load()
{
    update();

    // Saving a score (also as a new one) puts it in the recent files
    recentFilesController()->recentFilesListChanged().onNotify(this, [this]() {
        update();
    });
}

QList<int> LibraryScoresModel::nonScoreItemIndices() const
{
    if (!m_showCreateNew) {
        return { rowCount() - 1 };
    }
    return { 0, rowCount() - 1 };
}

QString LibraryScoresModel::folder() const
{
    return m_folder;
}

void LibraryScoresModel::setFolder(const QString& folder)
{
    if (m_folder == folder) {
        return;
    }
    m_folder = folder;
    emit folderChanged();
}

bool LibraryScoresModel::showCreateNew() const
{
    return m_showCreateNew;
}

void LibraryScoresModel::setShowCreateNew(bool show)
{
    if (m_showCreateNew == show) {
        return;
    }
    m_showCreateNew = show;
    emit folderChanged();
}

void LibraryScoresModel::reload()
{
    update();
}

void LibraryScoresModel::update()
{
    // The scores folder alone: its subfolders are other folders (the Drive folder, ...).
    // Another folder with its subfolders, but not the hidden ones (the sync's own files)
    const bool otherFolder = !m_folder.isEmpty();
    const io::path_t folder = otherFolder ? io::path_t(m_folder) : configuration()->userProjectsPath();
    RetVal<io::paths_t> files = fileSystem()->scanFiles(folder, SCORE_FILTERS,
                                                        otherFolder ? io::ScanMode::FilesInCurrentDirAndSubdirs
                                                        : io::ScanMode::FilesInCurrentDir);

    std::vector<std::pair<QDateTime, io::path_t> > scores;
    for (const io::path_t& path : files.val) {
        if (otherFolder && path.toQString().mid(m_folder.size()).contains(QStringLiteral("/."))) {
            continue;
        }
        scores.emplace_back(fileSystem()->lastModified(path).toQDateTime(), path);
    }

    // The latest first
    std::sort(scores.begin(), scores.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });

    std::vector<QVariantMap> items;
    items.reserve(scores.size() + 2);

    if (m_showCreateNew) {
        QVariantMap addItem;
        addItem[NAME_KEY] = muse::qtrc("project", "New score");
        addItem[IS_CREATE_NEW_KEY] = true;
        addItem[IS_NO_RESULTS_FOUND_KEY] = false;
        addItem[IS_CLOUD_KEY] = false;
        items.push_back(addItem);
    }

    for (const auto& [modified, path] : scores) {
        const std::string suffix = io::suffix(path);
        RetVal<uint64_t> fileSize = fileSystem()->fileSize(path);

        QVariantMap obj;
        obj[NAME_KEY] = suffix == engraving::MSCZ ? io::completeBasename(path).toQString() : io::filename(path).toQString();
        obj[PATH_KEY] = path.toQString();
        obj[SUFFIX_KEY] = QString::fromStdString(suffix);
        obj[FILE_SIZE_KEY] = (fileSize.ret && fileSize.val > 0) ? DataFormatter::formatFileSize(fileSize.val).toQString() : QString();
        obj[IS_CLOUD_KEY] = false;
        obj[TIME_SINCE_MODIFIED_KEY] = DataFormatter::formatTimeSince(Date::fromQDate(modified.date())).toQString();
        obj[IS_CREATE_NEW_KEY] = false;
        obj[IS_NO_RESULTS_FOUND_KEY] = false;
        items.push_back(obj);
    }

    QVariantMap noResultsFoundItem;
    noResultsFoundItem[NAME_KEY] = "";
    noResultsFoundItem[IS_CREATE_NEW_KEY] = false;
    noResultsFoundItem[IS_NO_RESULTS_FOUND_KEY] = true;
    noResultsFoundItem[IS_CLOUD_KEY] = false;
    items.push_back(noResultsFoundItem);

    if (m_items == items) {
        return;
    }

    beginResetModel();
    m_items = items;
    endResetModel();
    emit rowCountChanged();
}

QString LibraryScoresModel::scoreName(const QString& scorePath) const
{
    return io::completeBasename(scorePath).toQString();
}

bool LibraryScoresModel::isOpen(const io::path_t& path) const
{
    const INotationProjectPtr project = globalContext()->currentProject();
    return project && project->path() == path;
}

void LibraryScoresModel::showScoreIsOpenMessage()
{
    interactive()->info(muse::trc("project", "This score is open"),
                        IInteractive::Text(muse::trc("project", "Close the score first.")));
}

QString LibraryScoresModel::renameScore(const QString& scorePath, const QString& newName)
{
    const QString name = newName.trimmed();
    if (name.isEmpty() || name.contains('/') || name.contains('\\')) {
        return muse::qtrc("project", "Enter a name that has no slashes.");
    }

    const io::path_t path = scorePath;
    if (isOpen(path)) {
        showScoreIsOpenMessage();
        return QString();
    }

    const io::path_t newPath = io::dirpath(path) + "/" + name + "." + io::suffix(path);
    if (newPath == path) {
        return QString();
    }

    if (fileSystem()->exists(newPath)) {
        return muse::qtrc("project", "There is a score with this name already.");
    }

    Ret ret = fileSystem()->move(path, newPath);
    if (!ret) {
        LOGE() << "Could not rename " << path << ": " << ret.toString();
        return muse::qtrc("project", "The score could not be renamed.");
    }

    // The recent files list keeps the score under its new name
    RecentFile renamed;
    renamed.path = newPath;
    recentFilesController()->moveRecentFile(path, renamed);

    update();
    return QString();
}

void LibraryScoresModel::duplicateScore(const QString& scorePath)
{
    const io::path_t path = scorePath;
    const io::path_t folder = io::dirpath(path);
    const QString suffix = QString::fromStdString(io::suffix(path));
    const QString baseName = io::completeBasename(path).toQString();

    io::path_t copyPath;
    for (int i = 1;; ++i) {
        //: The name of a copy of a score. %1 is the name of the score; %2 is the number of the copy (shown from the second copy)
        const QString name = i == 1 ? muse::qtrc("project", "%1 (copy)").arg(baseName)
                             : muse::qtrc("project", "%1 (copy %2)").arg(baseName).arg(i);
        copyPath = folder + "/" + name + "." + suffix;
        if (!fileSystem()->exists(copyPath)) {
            break;
        }
    }

    Ret ret = fileSystem()->copy(path, copyPath);
    if (!ret) {
        LOGE() << "Could not copy " << path << ": " << ret.toString();
        interactive()->error(muse::trc("project", "The score could not be duplicated."), IInteractive::Text(ret.text()));
        return;
    }

    update();
}

void LibraryScoresModel::deleteScore(const QString& scorePath)
{
    const io::path_t path = scorePath;
    if (isOpen(path)) {
        showScoreIsOpenMessage();
        return;
    }

    //: %1 is the name of a score
    const std::string question = muse::qtrc("project", "Delete “%1”?").arg(scoreName(scorePath)).toStdString();
    const int deleteButton = int(IInteractive::Button::CustomButton) + 1;
    const IInteractive::ButtonDatas buttons {
        interactive()->buttonData(IInteractive::Button::Cancel),
        IInteractive::ButtonData(deleteButton, muse::trc("global", "Delete"), /*accent*/ true, /*leftSide*/ false,
                                 IInteractive::ButtonRole::DestructiveRole)
    };

    auto promise = interactive()->warning(question, IInteractive::Text(muse::trc("project", "The score cannot be restored.")),
                                          buttons, int(IInteractive::Button::Cancel));

    promise.onResolve(this, [this, path, deleteButton](const IInteractive::Result& res) {
        if (!res.isButton(deleteButton)) {
            return;
        }

        Ret ret = fileSystem()->remove(path);
        if (!ret) {
            LOGE() << "Could not delete " << path << ": " << ret.toString();
            interactive()->error(muse::trc("project", "The score could not be deleted."), IInteractive::Text(ret.text()));
            return;
        }

        recentFilesController()->removeRecentFile(path);
        update();
    });
}
