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

import QtQuick

import Muse.Ui
import Muse.UiComponents
import MuseScore.Project

//! The scores in the scores folder, with actions to duplicate, rename and delete them
ScoresView {
    id: root

    //! Another folder than the scores folder (the Drive folder), with its subfolders
    property string folder: ""
    property bool showCreateNew: true
    //! More actions for each score, handled by the user of the view (extraScoreActionRequested)
    property var moreScoreActions: []

    signal extraScoreActionRequested(string actionId, string scorePath)

    function reload() {
        libraryScoresModel.reload()
    }

    LibraryScoresModel {
        id: libraryScoresModel

        folder: root.folder
        showCreateNew: root.showCreateNew
    }

    Component.onCompleted: {
        libraryScoresModel.load()
    }

    readonly property var scoreActions: [
        { id: "duplicate", title: qsTrc("project", "Duplicate") },
        { id: "rename", title: qsTrc("project", "Rename…") },
        { id: "delete", title: qsTrc("global", "Delete") }
    ].concat(root.moreScoreActions)

    function handleScoreAction(actionId, scorePath) {
        switch (actionId) {
        case "duplicate":
            libraryScoresModel.duplicateScore(scorePath)
            break
        case "rename":
            renameDialog.scorePath = scorePath
            renameDialog.name = libraryScoresModel.scoreName(scorePath)
            renameDialog.errorText = ""
            renameDialog.open()
            break
        case "delete":
            Qt.callLater(libraryScoresModel.deleteScore, scorePath)
            break
        default:
            root.extraScoreActionRequested(actionId, scorePath)
            break
        }
    }

    StyledDialogView {
        id: renameDialog

        property string scorePath: ""
        property string name: ""
        property string errorText: ""

        title: qsTrc("project", "Rename score")

        contentWidth: 400
        contentHeight: renameContent.implicitHeight
        margins: 20

        function rename() {
            const error = libraryScoresModel.renameScore(renameDialog.scorePath, renameDialog.name)
            if (error === "") {
                renameDialog.close()
            } else {
                renameDialog.errorText = error
            }
        }

        onNavigationActivateRequested: {
            nameField.navigation.requestActive()
            nameField.ensureActiveFocus()
        }

        Column {
            id: renameContent

            anchors.left: parent.left
            anchors.right: parent.right

            spacing: 16

            NavigationPanel {
                id: renameNavPanel
                name: "RenameScore"
                section: renameDialog.navigationSection
                order: 1
            }

            TextInputField {
                id: nameField

                width: parent.width

                currentText: renameDialog.name
                hint: qsTrc("project", "Score name")

                navigation.panel: renameNavPanel
                navigation.row: 1

                onTextChanged: function(newTextValue) {
                    renameDialog.name = newTextValue
                    renameDialog.errorText = ""
                }

                onAccepted: {
                    renameDialog.rename()
                }
            }

            StyledTextLabel {
                width: parent.width

                visible: renameDialog.errorText !== ""
                text: renameDialog.errorText
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.WordWrap
            }

            ButtonBox {
                width: parent.width

                buttons: [ ButtonBoxModel.Cancel, ButtonBoxModel.Ok ]

                navigationPanel.section: renameDialog.navigationSection
                navigationPanel.order: 2

                onStandardButtonClicked: function(buttonId) {
                    if (buttonId === ButtonBoxModel.Ok) {
                        renameDialog.rename()
                    } else {
                        renameDialog.close()
                    }
                }
            }
        }
    }

    sourceComponent: root.viewType === ScoresPageModel.List ? listComp : gridComp

    Component {
        id: gridComp

        ScoresGridView {
            anchors.fill: parent

            model: libraryScoresModel
            searchText: root.searchText
            extraScoreActions: root.scoreActions

            isNoResultsMessageAllowed: false // provided by the model instead

            backgroundColor: root.backgroundColor
            sideMargin: root.sideMargin

            navigation.section: root.navigationSection
            navigation.order: root.navigationOrder
            navigation.name: "LibraryScoresGrid"
            navigation.accessible.name: qsTrc("project", "Library scores grid")

            onCreateNewScoreRequested: {
                root.createNewScoreRequested()
            }

            onOpenScoreRequested: function(scorePath, displayName) {
                root.openScoreRequested(scorePath, displayName)
            }

            onRevealInFileBrowserRequested: function(scorePath) {
                root.revealInFileBrowserRequested(scorePath)
            }

            onViewOnlineRequested: function(scoreId) {
                root.viewOnlineRequested(scoreId)
            }

            onScoreActionRequested: function(actionId, scorePath) {
                root.handleScoreAction(actionId, scorePath)
            }
        }
    }

    Component {
        id: listComp

        ScoresListView {
            id: list

            anchors.fill: parent

            model: libraryScoresModel
            searchText: root.searchText
            extraScoreActions: root.scoreActions

            backgroundColor: root.backgroundColor
            sideMargin: root.sideMargin

            showNewScoreItem: true

            navigation.section: root.navigationSection
            navigation.order: root.navigationOrder
            navigation.name: "LibraryScoresList"
            navigation.accessible.name: qsTrc("project", "Library scores list")

            onCreateNewScoreRequested: {
                root.createNewScoreRequested()
            }

            onOpenScoreRequested: function(scorePath, displayName) {
                root.openScoreRequested(scorePath, displayName)
            }

            onRevealInFileBrowserRequested: function(scorePath) {
                root.revealInFileBrowserRequested(scorePath)
            }

            onViewOnlineRequested: function(scoreId) {
                root.viewOnlineRequested(scoreId)
            }

            onScoreActionRequested: function(actionId, scorePath) {
                root.handleScoreAction(actionId, scorePath)
            }

            columns: [
                ScoresListView.ColumnItem {
                    id: modifiedColumn

                    //: Stands for "Last time that this score was modified".
                    //: Used as the header of this column in the scores list.
                    header: qsTrc("project", "Modified")

                    width: function (parentWidth) {
                        let parentWidthExclusingSpacing = parentWidth - list.columns.length * list.view.columnSpacing;
                        return 0.25 * parentWidthExclusingSpacing
                    }

                    delegate: StyledTextLabel {
                        id: modifiedLabel
                        text: score.timeSinceModified ?? ""

                        font.capitalization: Font.AllUppercase
                        horizontalAlignment: Text.AlignLeft

                        NavigationFocusBorder {
                            navigationCtrl: NavigationControl {
                                name: "ModifiedLabel"
                                panel: navigationPanel
                                row: navigationRow
                                column: navigationColumnStart
                                enabled: modifiedLabel.visible && modifiedLabel.enabled && !modifiedLabel.isEmpty
                                accessible.name: modifiedColumn.header + ": " + modifiedLabel.text
                                accessible.role: MUAccessible.StaticText

                                onActiveChanged: {
                                    if (active) {
                                        listItem.scrollIntoView()
                                    }
                                }
                            }

                            anchors.margins: -radius
                            radius: 2 + border.width
                        }
                    }
                },

                ScoresListView.ColumnItem {
                    id: sizeColumn
                    header: qsTrc("global", "Size", "file size")

                    width: function (parentWidth) {
                        let parentWidthExclusingSpacing = parentWidth - list.columns.length * list.view.columnSpacing;
                        return 0.15 * parentWidthExclusingSpacing
                    }

                    delegate: StyledTextLabel {
                        id: sizeLabel
                        text: Boolean(score.fileSize) ? score.fileSize : "-"

                        font: ui.theme.largeBodyFont
                        horizontalAlignment: Text.AlignLeft

                        NavigationFocusBorder {
                            navigationCtrl: NavigationControl {
                                name: "SizeLabel"
                                panel: navigationPanel
                                row: navigationRow
                                column: navigationColumnStart
                                enabled: sizeLabel.visible && sizeLabel.enabled && !sizeLabel.isEmpty
                                accessible.name: sizeColumn.header + ": " + (Boolean(score.fileSize) ? score.fileSize : qsTrc("global", "Unknown"))
                                accessible.role: MUAccessible.StaticText

                                onActiveChanged: {
                                    if (active) {
                                        listItem.scrollIntoView()
                                    }
                                }
                            }

                            anchors.margins: -radius
                            radius: 2 + border.width
                        }
                    }
                }
            ]
        }
    }
}
