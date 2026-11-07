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
import QtQuick.Layouts

import Muse.Ui
import Muse.UiComponents
import MuseScore.Project

//! The scores of the shared Google Drive folder: sign-in, the folder, the state of the sync, and the scores
Item {
    id: root

    property int viewType: ScoresPageModel.Grid
    property string searchText: ""
    property color backgroundColor: ui.theme.backgroundSecondaryColor
    property real sideMargin: 46

    property NavigationSection navigationSection: null
    property int navigationOrder: 0

    //! Set by the Scores page: its model has the actions
    property var driveModel: null

    signal openScoreRequested(var scorePath, var displayName)
    signal revealInFileBrowserRequested(var scorePath)

    readonly property string state_: root.driveModel ? root.driveModel.state : "notSignedIn"
    readonly property bool isReady: root.state_ === "idle" || root.state_ === "syncing"

    NavigationPanel {
        id: navPanel
        name: "DriveHeader"
        section: root.navigationSection
        order: root.navigationOrder
        direction: NavigationPanel.Horizontal
        accessible.name: qsTrc("project", "Google Drive")
    }

    Connections {
        target: root.driveModel

        function onSyncFinished() {
            if (scoresLoader.item) {
                scoresLoader.item.reload()
            }
        }
    }

    ColumnLayout {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.leftMargin: root.sideMargin
        anchors.right: parent.right
        anchors.rightMargin: root.sideMargin

        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            StyledTextLabel {
                Layout.fillWidth: true

                text: root.driveModel ? root.driveModel.statusText : ""
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.WordWrap
                maximumLineCount: 3
            }

            FlatButton {
                visible: root.state_ === "notSignedIn"
                accentButton: true
                text: qsTrc("project", "Sign in with Google")

                navigation.panel: navPanel
                navigation.column: 1

                onClicked: root.driveModel.signIn()
            }

            FlatButton {
                visible: root.state_ === "signingIn"
                text: qsTrc("global", "Cancel")

                navigation.panel: navPanel
                navigation.column: 2

                onClicked: root.driveModel.cancelSignIn()
            }

            FlatButton {
                visible: root.isReady
                enabled: root.state_ !== "syncing"
                icon: IconCode.UPDATE
                text: qsTrc("project", "Sync")
                orientation: Qt.Horizontal

                navigation.panel: navPanel
                navigation.column: 3

                onClicked: root.driveModel.syncNow()
            }

            MenuButton {
                visible: root.state_ !== "notSignedIn" && root.state_ !== "signingIn"

                navigation.panel: navPanel
                navigation.column: 4

                menuModel: [
                    { id: "folder", title: qsTrc("project", "Change Drive folder…") },
                    { id: "signOut", title: qsTrc("project", "Sign out of Google") }
                ]

                onHandleMenuItem: function(itemId) {
                    if (itemId === "folder") {
                        folderField.currentText = ""
                        folderRow.link = ""
                        folderRow.forceShown = true
                    } else if (itemId === "signOut") {
                        root.driveModel.signOut()
                    }
                }
            }
        }

        RowLayout {
            id: folderRow

            property bool forceShown: false
            property string link: ""

            Layout.fillWidth: true
            visible: root.state_ === "noFolder" || forceShown
            spacing: 12

            TextInputField {
                id: folderField

                Layout.fillWidth: true
                hint: qsTrc("project", "Link of the Drive folder")

                navigation.panel: navPanel
                navigation.column: 5

                onTextChanged: function(newTextValue) {
                    folderRow.link = newTextValue
                    folderError.text = ""
                }

                onAccepted: folderRow.apply()
            }

            FlatButton {
                text: qsTrc("global", "OK")
                accentButton: true

                navigation.panel: navPanel
                navigation.column: 6

                onClicked: folderRow.apply()
            }

            function apply() {
                const error = root.driveModel.setFolder(folderRow.link)
                folderError.text = error
                if (error === "") {
                    folderRow.forceShown = false
                }
            }
        }

        StyledTextLabel {
            id: folderError

            Layout.fillWidth: true
            visible: folderRow.visible && text !== ""
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: root.isReady && root.driveModel ? root.driveModel.problems : []

            StyledTextLabel {
                required property var modelData

                Layout.fillWidth: true
                text: "• " + modelData
                horizontalAlignment: Text.AlignLeft
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                opacity: 0.8
            }
        }
    }

    Loader {
        id: scoresLoader

        anchors.top: header.bottom
        anchors.topMargin: 16
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        active: root.isReady

        sourceComponent: LibraryScoresView {
            folder: root.driveModel.rootPath
            showCreateNew: false

            viewType: root.viewType
            searchText: root.searchText
            backgroundColor: root.backgroundColor
            sideMargin: root.sideMargin

            navigationSection: root.navigationSection
            navigationOrder: root.navigationOrder + 1

            onOpenScoreRequested: function(scorePath, displayName) {
                root.openScoreRequested(scorePath, displayName)
            }

            onRevealInFileBrowserRequested: function(scorePath) {
                root.revealInFileBrowserRequested(scorePath)
            }
        }
    }
}
