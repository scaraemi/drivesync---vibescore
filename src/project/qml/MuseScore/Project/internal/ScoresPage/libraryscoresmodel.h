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

#include <qqmlintegration.h>

#include "abstractscoresmodel.h"

#include "async/asyncable.h"
#include "modularity/ioc.h"

#include "context/iglobalcontext.h"
#include "global/iinteractive.h"
#include "io/ifilesystem.h"
#include "iprojectconfiguration.h"
#include "irecentfilescontroller.h"

namespace mu::project {
//! The scores in the scores folder ("My scores"): on mobile systems, the app's library of scores,
//! which no file manager shows. Also renames, duplicates and deletes them.
class LibraryScoresModel : public AbstractScoresModel, public muse::async::Asyncable, public muse::Contextable
{
    Q_OBJECT
    QML_ELEMENT

    //! Another folder than the scores folder (e.g. the Drive folder), with its subfolders. Set before load()
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
    //! The "New score" item (it creates the score in the scores folder)
    Q_PROPERTY(bool showCreateNew READ showCreateNew WRITE setShowCreateNew NOTIFY folderChanged)

    muse::GlobalInject<IProjectConfiguration> configuration;
    muse::GlobalInject<muse::io::IFileSystem> fileSystem;
    muse::ContextInject<IRecentFilesController> recentFilesController = { this };
    muse::ContextInject<context::IGlobalContext> globalContext = { this };
    muse::ContextInject<muse::IInteractive> interactive = { this };

public:
    explicit LibraryScoresModel(QObject* parent = nullptr);

    void load() override;

    QString folder() const;
    void setFolder(const QString& folder);
    bool showCreateNew() const;
    void setShowCreateNew(bool show);

    //! Reads the folder again (e.g. after a sync)
    Q_INVOKABLE void reload();
    QList<int> nonScoreItemIndices() const override;

    //! The name of the score without the extension
    Q_INVOKABLE QString scoreName(const QString& scorePath) const;

    //! Returns an error message, or an empty string when it worked
    Q_INVOKABLE QString renameScore(const QString& scorePath, const QString& newName);
    Q_INVOKABLE void duplicateScore(const QString& scorePath);
    Q_INVOKABLE void deleteScore(const QString& scorePath);

signals:
    void folderChanged();

private:
    void update();
    bool isOpen(const muse::io::path_t& path) const;
    void showScoreIsOpenMessage();

    QString m_folder;
    bool m_showCreateNew = true;
};
}
