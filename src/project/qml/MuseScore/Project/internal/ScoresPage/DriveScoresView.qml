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

    Binding {
        target: root.driveModel
        property: "searchText"
        value: root.searchText
        when: root.driveModel !== null
    }

    RowLayout {
        id: folderBar

        anchors.top: header.bottom
        anchors.topMargin: 12
        anchors.left: parent.left
        anchors.leftMargin: root.sideMargin
        anchors.right: parent.right
        anchors.rightMargin: root.sideMargin

        visible: root.isReady && root.driveModel.currentFolder !== "" && root.searchText === ""
        height: visible ? implicitHeight : 0
        spacing: 8

        FlatButton {
            icon: IconCode.ARROW_LEFT
            transparent: true
            toolTipTitle: qsTrc("global", "Back")

            navigation.panel: navPanel
            navigation.column: 7

            onClicked: root.driveModel.goUp()
        }

        StyledTextLabel {
            Layout.fillWidth: true
            text: root.driveModel ? root.driveModel.currentFolderName : ""
            font: ui.theme.largeBodyBoldFont
            horizontalAlignment: Text.AlignLeft
        }
    }

    NavigationPanel {
        id: listPanel
        name: "DriveScores"
        section: root.navigationSection
        order: root.navigationOrder + 1
        direction: NavigationPanel.Vertical
        accessible.name: qsTrc("project", "Scores on Drive")
    }

    StyledListView {
        id: list

        anchors.top: folderBar.visible ? folderBar.bottom : header.bottom
        anchors.topMargin: 12
        anchors.left: parent.left
        anchors.leftMargin: root.sideMargin
        anchors.right: parent.right
        anchors.rightMargin: root.sideMargin
        anchors.bottom: parent.bottom

        visible: root.isReady
        model: root.isReady ? root.driveModel.items : []
        spacing: 2

        delegate: ListItemBlank {
            id: itemDelegate

            required property var modelData
            required property int index

            width: ListView.view.width
            height: 64
            radius: 4

            navigation.panel: listPanel
            navigation.row: index
            navigation.accessible.name: modelData.name

            onClicked: root.driveModel.openItem(modelData.path)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 4
                spacing: 12

                StyledIconLabel {
                    Layout.preferredWidth: 28
                    iconCode: itemDelegate.modelData.isFolder ? IconCode.OPEN_FILE
                              : itemDelegate.modelData.locked ? IconCode.LOCK_CLOSED
                              : itemDelegate.modelData.onDevice ? IconCode.SCORE : IconCode.CLOUD
                    font.pixelSize: 20
                    opacity: itemDelegate.modelData.isFolder || itemDelegate.modelData.onDevice ? 1.0 : 0.7
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    StyledTextLabel {
                        Layout.fillWidth: true
                        text: itemDelegate.modelData.name
                        font: ui.theme.bodyBoldFont
                        horizontalAlignment: Text.AlignLeft
                    }

                    StyledTextLabel {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: itemDelegate.modelData.subtitle
                        horizontalAlignment: Text.AlignLeft
                        opacity: 0.7
                    }

                    StyledTextLabel {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: itemDelegate.modelData.lockText
                        horizontalAlignment: Text.AlignLeft
                        color: ui.theme.accentColor
                    }
                }

                MenuButton {
                    visible: !itemDelegate.modelData.isFolder

                    navigation.panel: listPanel
                    navigation.row: itemDelegate.index
                    navigation.column: 1

                    menuModel: {
                        const d = itemDelegate.modelData
                        let actions = []
                        if (d.onDevice && !d.notUploaded) {
                            actions.push({ id: "removeFromDevice", title: qsTrc("project", "Remove from this device") })
                        }
                        if (d.locked && (d.lockedHere || d.lockStale)) {
                            actions.push({ id: "unlock", title: qsTrc("project", "Unlock") })
                        }
                        actions.push({ id: "delete", title: qsTrc("project", "Delete from Drive…") })
                        return actions
                    }

                    onHandleMenuItem: function(itemId) {
                        const path = itemDelegate.modelData.path
                        if (itemId === "removeFromDevice") {
                            Qt.callLater(root.driveModel.removeFromDevice, path)
                        } else if (itemId === "unlock") {
                            Qt.callLater(root.driveModel.unlock, path)
                        } else if (itemId === "delete") {
                            Qt.callLater(root.driveModel.deleteFromDrive, path)
                        }
                    }
                }
            }
        }

        StyledTextLabel {
            anchors.centerIn: parent
            visible: list.count === 0 && root.isReady
            text: root.searchText !== "" ? qsTrc("project", "No scores found")
                                         : qsTrc("project", "No scores in this folder")
            opacity: 0.7
        }
    }
}
