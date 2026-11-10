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
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window

import Muse.Ui
import Muse.UiComponents
import MuseScore.AppShell
import MuseScore.NotationScene
import MuseScore.Palette
import MuseScore.Inspector
import MuseScore.InstrumentsScene
import MuseScore.Playback

//! The score page of the mobile shell:
//! top bar (Home, title, undo/redo, playback, panels, app menu), note input toolbar,
//! the score with the side sheet (palettes, layout, properties, ...) and the bottom sheet
//! (mixer, piano keyboard, timeline, percussion), status bar.
//! Panels are opened and closed like docks (View menu, dock actions; see MobileShellView).
Item {
    id: root

    property var shell: null
    property string scoreTitle: ""
    //! Panels open over the score, one at a time
    property bool isPhone: false
    //! Phones in portrait
    property bool isCompact: false

    //! The toolbars of the top bar that fit
    readonly property bool hasRoomForPlaybackToolBar: width >= 1000
    readonly property bool hasRoomForNotationToolBar: width >= 1150

    property var notationView: null

    //! While text on the score is edited: the bar over the keyboard
    readonly property bool isEditingText: root.shell ? root.shell.isEditingText : false

    //! Phones: the score takes the whole screen (no system bars) while the page is shown; the system bars come
    //! back when the keyboard is needed, so that Android keeps the text being written above the keyboard
    readonly property bool wantsFullScreen: root.isPhone && root.visible && !root.isEditingText && !Qt.inputMethod.visible
    property int visibilityBeforeFullScreen: -1

    onWantsFullScreenChanged: Qt.callLater(root.applyFullScreen)

    function applyFullScreen() {
        const win = root.Window.window
        if (!win) {
            return
        }
        if (root.wantsFullScreen) {
            if (win.visibility !== Window.FullScreen) {
                root.visibilityBeforeFullScreen = win.visibility
                win.visibility = Window.FullScreen
            }
        } else if (root.visibilityBeforeFullScreen >= 0) {
            win.visibility = root.visibilityBeforeFullScreen
            root.visibilityBeforeFullScreen = -1
        }
    }

    //! Phones: less height for the bars over the score
    readonly property int topBarHeight: root.isPhone ? 44 : 52
    readonly property int noteInputBarHeight: root.isPhone ? 44 : 48

    //! The part of the page under the keyboard. Android makes the window smaller when the keyboard comes
    //! (adjustResize: nothing under it); on iOS the keyboard covers the window.
    readonly property real keyboardOverlap: {
        if (!Qt.inputMethod.visible || Qt.inputMethod.keyboardRectangle.height <= 0) {
            return 0
        }
        const pageBottom = root.mapToItem(null, 0, root.height).y
        return Math.max(0, pageBottom - Qt.inputMethod.keyboardRectangle.y)
    }

    readonly property string sideGroup: "side"
    readonly property string bottomGroup: "bottom"

    NotationPageModel {
        id: pageModel
    }

    AppMenuModel {
        id: appMenuModel
    }

    readonly property var sidePanels: [
        { "name": pageModel.palettesPanelName(), "title": qsTrc("appshell", "Palettes"), "component": palettesComp },
        { "name": pageModel.layoutPanelName(), "title": qsTrc("appshell", "Layout"), "component": layoutComp },
        { "name": pageModel.inspectorPanelName(), "title": qsTrc("appshell", "Properties"), "component": inspectorComp },
        { "name": pageModel.selectionFiltersPanelName(), "title": qsTrc("appshell", "Selection filter"), "component": selectionFilterComp },
        { "name": pageModel.undoHistoryPanelName(), "title": qsTrc("appshell", "History"), "component": undoHistoryComp }
    ]

    //! Not a dock of the desktop app
    readonly property string keypadPanelName: "noteInputKeypadPanel"

    readonly property var bottomPanels: [
        { "name": root.keypadPanelName, "title": qsTrc("notation", "Keypad"), "component": keypadComp },
        { "name": pageModel.mixerPanelName(), "title": qsTrc("appshell", "Mixer"), "component": mixerComp },
        { "name": pageModel.pianoKeyboardPanelName(), "title": qsTrc("appshell", "Piano keyboard"), "component": pianoKeyboardComp },
        { "name": pageModel.timelinePanelName(), "title": qsTrc("appshell", "Timeline"), "component": timelineComp },
        { "name": pageModel.percussionPanelName(), "title": qsTrc("appshell", "Percussion"), "component": percussionComp }
    ]

    // Phones: the keypad comes with note input (unless another bottom panel is open)
    property bool keypadOpenedForNoteInput: false

    Connections {
        target: root.shell

        function onIsNoteInputModeChanged() {
            if (!root.isPhone) {
                return
            }

            if (root.shell.isNoteInputMode) {
                if (!root.isOpen(root.keypadPanelName) && !bottomSheet.isOpen && !sideSheet.isOpen) {
                    root.shell.setDockOpen(root.keypadPanelName, true)
                    root.keypadOpenedForNoteInput = true
                }
            } else if (root.keypadOpenedForNoteInput) {
                root.shell.setDockOpen(root.keypadPanelName, false)
                root.keypadOpenedForNoteInput = false
            }
        }
    }

    function isOpen(dockName) {
        return root.shell && root.shell.openDocks.indexOf(dockName) >= 0
    }

    Component.onDestruction: {
        touchModifiersModel.clear()

        const win = root.Window.window
        if (win && root.visibilityBeforeFullScreen >= 0) {
            win.visibility = root.visibilityBeforeFullScreen
        }
    }

    //! Back key (see MobileShell.qml)
    function closeOverlay() {
        if (appMenuDrawer.visible) {
            appMenuDrawer.back()
            return true
        }

        return false
    }

    Component.onCompleted: {
        for (const panel of root.sidePanels) {
            // Tablets show the palettes beside the score from the start
            root.shell.registerDock(panel.name, root.sideGroup, !root.isPhone && panel.name === pageModel.palettesPanelName())
        }

        for (const panel of root.bottomPanels) {
            root.shell.registerDock(panel.name, root.bottomGroup, false)
        }

        root.shell.registerDock(pageModel.drumsetPanelName(), "drumset", false)

        root.shell.registerDock(pageModel.noteInputBarName(), "", true)
        root.shell.registerDock(pageModel.playbackToolBarName(), "", true)
        root.shell.registerDock(pageModel.undoRedoToolBarName(), "", true)
        root.shell.registerDock(pageModel.notationToolBarName(), "", true)
        root.shell.registerDock(pageModel.extensionsToolBarName(), "", false)
        // Phones have no room for the status bar (zoom and view mode are in the View menu)
        root.shell.registerDock(pageModel.statusBarName(), "", !root.isPhone)

        appMenuModel.load()
        Qt.callLater(pageModel.init)
        Qt.callLater(root.applyFullScreen)
    }

    NavigationSection {
        id: topBarNavSec
        name: "TopTool"
        order: 1
    }

    NavigationSection {
        id: noteInputNavSec
        name: "NoteInputSection"
        order: 2
    }

    NavigationSection {
        id: sideSheetNavSec
        name: "NavigationLeftPanel"
        order: 4
    }

    NavigationSection {
        id: bottomSheetNavSec
        name: "NavigationBottomPanel"
        order: 7
    }

    // Top bar

    Rectangle {
        id: topBar

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        // Full screen: not under a camera cutout
        anchors.topMargin: root.SafeArea.margins.top
        anchors.leftMargin: root.SafeArea.margins.left
        anchors.rightMargin: root.SafeArea.margins.right

        height: root.topBarHeight

        color: ui.theme.backgroundSecondaryColor

        NavigationPanel {
            id: topBarNavPanel
            name: "MobileTopBar"
            section: topBarNavSec
            order: 1
            direction: NavigationPanel.Horizontal
        }

        FlatButton {
            id: homeButton

            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter

            icon: IconCode.ARROW_LEFT
            transparent: true
            toolTipTitle: qsTrc("appshell", "Home")

            navigation.panel: topBarNavPanel
            navigation.column: 0

            onClicked: {
                api.launcher.open("musescore://home")
            }
        }

        StyledTextLabel {
            id: titleLabel

            anchors.left: homeButton.right
            anchors.leftMargin: 8
            anchors.right: actions.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter

            horizontalAlignment: Text.AlignLeft
            font: ui.theme.largeBodyBoldFont
            text: root.scoreTitle
        }

        Row {
            id: actions

            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter

            spacing: 4

            FlatButton {
                anchors.verticalCenter: parent.verticalCenter

                // When there is no room for the playback toolbar
                visible: !playbackToolBar.visible
                // Not while the sounds of the score are loaded (a while for a big score): the tap would do nothing
                enabled: root.shell.isPlayAllowed

                icon: root.shell.isPlaying ? IconCode.PAUSE : IconCode.PLAY
                transparent: true
                toolTipTitle: root.shell.isPlaying ? qsTrc("action", "Pause") : qsTrc("action", "Play")

                navigation.panel: topBarNavPanel
                navigation.column: 1

                onClicked: {
                    root.shell.dispatch("play")
                }
            }

            UndoRedoToolBar {
                anchors.verticalCenter: parent.verticalCenter

                visible: root.isOpen(pageModel.undoRedoToolBarName())

                navigationPanel.section: topBarNavSec
                navigationPanel.order: 5
            }

            PlaybackToolBar {
                id: playbackToolBar

                anchors.verticalCenter: parent.verticalCenter

                visible: root.hasRoomForPlaybackToolBar && root.isOpen(pageModel.playbackToolBarName())

                navigationPanelSection: topBarNavSec
                navigationPanelOrder: 3
            }

            NotationToolBar {
                anchors.verticalCenter: parent.verticalCenter

                visible: root.hasRoomForNotationToolBar && root.isOpen(pageModel.notationToolBarName())

                navigationPanel.section: topBarNavSec
                navigationPanel.order: 2
            }

            FlatButton {
                id: panelsButton

                anchors.verticalCenter: parent.verticalCenter

                text: qsTrc("appshell", "Panels")
                orientation: Qt.Horizontal
                isNarrow: true
                transparent: true

                navigation.panel: topBarNavPanel
                navigation.column: 10

                onClicked: {
                    panelsMenu.toggleOpened(panelsMenu.items())
                }

                StyledMenuLoader {
                    id: panelsMenu

                    function items() {
                        const result = []
                        for (const panel of root.sidePanels.concat(root.bottomPanels)) {
                            result.push({
                                "id": panel.name,
                                "title": panel.title,
                                "checkable": true,
                                "checked": root.isOpen(panel.name),
                                "enabled": true
                            })
                        }
                        return result
                    }

                    onHandleMenuItem: function(itemId) {
                        root.shell.toggleDock(itemId)
                    }
                }
            }

            FlatButton {
                id: appMenuButton

                anchors.verticalCenter: parent.verticalCenter

                icon: IconCode.MENU_THREE_DOTS
                transparent: true
                toolTipTitle: qsTrc("appshell", "Menu")

                navigation.panel: topBarNavPanel
                navigation.column: 11

                onClicked: {
                    // Phones: one menu level at a time
                    if (root.isPhone) {
                        appMenuDrawer.open()
                    } else {
                        appMenu.toggleOpened(appMenuModel.items)
                    }
                }

                StyledMenuLoader {
                    id: appMenu

                    onHandleMenuItem: function(itemId) {
                        Qt.callLater(appMenuModel.handleMenuItem, itemId)
                    }
                }
            }
        }
    }

    // Note input toolbar

    Rectangle {
        id: noteInputBarContainer

        anchors.top: topBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right

        height: visible ? root.noteInputBarHeight : 0
        visible: root.isOpen(pageModel.noteInputBarName())

        color: ui.theme.backgroundPrimaryColor

        // The whole toolbar, scrolled sideways when it does not fit
        Flickable {
            id: noteInputBarFlickable

            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 4

            contentWidth: noteInputBar.width
            contentHeight: height
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            clip: true

            NoteInputBar {
                id: noteInputBar

                anchors.verticalCenter: parent.verticalCenter

                orientation: Qt.Horizontal
                maximumWidth: 4000
                maximumHeight: noteInputBarContainer.height

                navigationPanel.section: noteInputNavSec
                navigationPanel.order: 1
            }
        }

        SeparatorLine {
            anchors.bottom: parent.bottom
        }
    }

    // The score and the sheets

    Item {
        id: workArea

        anchors.top: noteInputBarContainer.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: textEditBar.visible ? textEditBar.top : statusBar.top

        MobileSheet {
            id: sideSheet

            // Tablets: beside the score; phones: over the lower part of the score (portrait) or its left side (landscape)
            anchors.top: root.isCompact ? undefined : parent.top
            anchors.left: parent.left
            anchors.bottom: parent.bottom

            width: root.isCompact ? parent.width : (root.isPhone ? 360 : 320)
            height: root.isCompact ? parent.height * 0.6 : parent.height
            visible: isOpen
            z: root.isPhone ? 2 : 0

            panels: root.sidePanels
            shell: root.shell
            navigationSection: sideSheetNavSec
        }

        SeparatorLine {
            anchors.left: sideSheet.right
            orientation: Qt.Vertical
            visible: sideSheet.visible && !root.isCompact
            z: sideSheet.z
        }

        NotationView {
            id: notationView

            anchors.top: parent.top
            anchors.left: sideSheet.visible && !root.isPhone ? sideSheet.right : parent.left
            anchors.leftMargin: sideSheet.visible && !root.isPhone ? 1 : 0
            anchors.right: parent.right
            anchors.bottom: drumsetPanelContainer.top

            name: "MainNotationView"

            isNavigatorVisible: pageModel.isNavigatorVisible
            isBraillePanelVisible: pageModel.isBraillePanelVisible
            isMainView: true

            Component.onCompleted: {
                root.notationView = notationView.paintView
                if (root.shell) {
                    root.shell.setDefaultNavigationControl(notationView.defaultNavigationControl)
                }
            }

            Component.onDestruction: {
                if (root.shell) {
                    root.shell.setDefaultNavigationControl(null)
                }
            }
        }

        // Shift and Ctrl for touches on the score

        TouchModifiersModel {
            id: touchModifiersModel
        }

        NavigationPanel {
            id: touchModifiersNavPanel
            name: "TouchModifiers"
            section: noteInputNavSec
            order: 2
            direction: NavigationPanel.Vertical
        }

        Grid {
            id: touchModifierToggles

            anchors.right: notationView.right
            anchors.bottom: notationView.bottom
            anchors.margins: 12

            // Side by side when the score has little height (phones in landscape)
            columns: notationView.height < 360 ? 2 : 1
            spacing: 8

            readonly property int buttonWidth: root.isPhone ? 56 : 64
            readonly property int buttonHeight: root.isPhone ? 40 : 44

            FlatButton {
                width: touchModifierToggles.buttonWidth
                height: touchModifierToggles.buttonHeight

                text: qsTrc("shortcuts", "Shift")
                accentButton: touchModifiersModel.shift
                toolTipTitle: qsTrc("mobile", "Shift")
                toolTipDescription: qsTrc("mobile", "While on, a tap extends the selection to a range and a drag selects what it surrounds")

                navigation.panel: touchModifiersNavPanel
                navigation.row: 0
                navigation.accessible.checked: touchModifiersModel.shift

                onClicked: {
                    touchModifiersModel.shift = !touchModifiersModel.shift
                }
            }

            FlatButton {
                width: touchModifierToggles.buttonWidth
                height: touchModifierToggles.buttonHeight

                text: qsTrc("shortcuts", "Ctrl")
                accentButton: touchModifiersModel.ctrl
                toolTipTitle: qsTrc("mobile", "Ctrl")
                toolTipDescription: qsTrc("mobile", "While on, a tap adds to the selection or removes from it")

                navigation.panel: touchModifiersNavPanel
                navigation.row: 1
                navigation.accessible.checked: touchModifiersModel.ctrl

                onClicked: {
                    touchModifiersModel.ctrl = !touchModifiersModel.ctrl
                }
            }
        }

        Item {
            id: drumsetPanelContainer

            anchors.left: notationView.left
            anchors.right: parent.right
            anchors.bottom: bottomSheet.visible ? bottomSheet.top : parent.bottom

            height: visible ? 64 : 0
            visible: root.isOpen(pageModel.drumsetPanelName())

            DrumsetPanel {
                anchors.fill: parent
                navigationSection: bottomSheetNavSec
                contentNavigationPanelOrderStart: 100
            }
        }

        MobileSheet {
            id: bottomSheet

            anchors.left: root.isPhone || !sideSheet.visible ? parent.left : sideSheet.right
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            readonly property real defaultHeight: root.isPhone ? parent.height * 0.6 : 320
            height: Math.min(preferredHeight > 0 ? preferredHeight : defaultHeight, parent.height)
            visible: isOpen
            z: 3

            panels: root.bottomPanels
            shell: root.shell
            navigationSection: bottomSheetNavSec

            SeparatorLine {
                anchors.top: parent.top
            }
        }
    }

    // Text editing: in place of the status bar, over the keyboard

    TextEditBar {
        id: textEditBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.keyboardOverlap

        visible: root.isEditingText

        shell: root.shell
    }

    // Status bar

    Rectangle {
        id: statusBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        height: visible ? 36 : 0
        visible: root.isOpen(pageModel.statusBarName()) && !root.isEditingText

        color: ui.theme.backgroundSecondaryColor

        NotationStatusBar {
            anchors.fill: parent
        }
    }

    AppMenuDrawer {
        id: appMenuDrawer

        onVisibleChanged: {
            if (!visible && root.shell) {
                Qt.callLater(root.shell.focusScoreIfNothingActive)
            }
        }

        anchors.fill: parent
        z: 100

        title: qsTrc("appshell", "Menu")
        menuModel: appMenuModel.items

        onHandleMenuItem: function(itemId) {
            Qt.callLater(appMenuModel.handleMenuItem, itemId)
        }
    }

    // Panels

    Component {
        id: palettesComp

        PalettesPanel {
            navigationSection: sideSheetNavSec
            navigationOrderStart: 1
        }
    }

    Component {
        id: layoutComp

        LayoutPanel {
            navigationSection: sideSheetNavSec
            navigationOrderStart: 1
        }
    }

    Component {
        id: inspectorComp

        InspectorForm {
            navigationSection: sideSheetNavSec
            navigationOrderStart: 1
            notationView: root.notationView
        }
    }

    Component {
        id: selectionFilterComp

        SelectionFilterPanel {
            navigationSection: sideSheetNavSec
            navigationOrderStart: 1
        }
    }

    Component {
        id: undoHistoryComp

        UndoHistoryPanel {
            navigationSection: sideSheetNavSec
            navigationOrderStart: 1
        }
    }

    Component {
        id: keypadComp

        NoteInputKeypad {
            shell: root.shell
            navigationSection: bottomSheetNavSec
            navigationOrderStart: 1
        }
    }

    Component {
        id: mixerComp

        MixerPanel {
            navigationSection: bottomSheetNavSec
            contentNavigationPanelOrderStart: 1
        }
    }

    Component {
        id: pianoKeyboardComp

        PianoKeyboardPanel {
            navigationSection: bottomSheetNavSec
            contentNavigationPanelOrderStart: 1
        }
    }

    Component {
        id: timelineComp

        Timeline {
            navigationSection: bottomSheetNavSec
            contentNavigationPanelOrderStart: 1
        }
    }

    Component {
        id: percussionComp

        PercussionPanel {
            navigationSection: bottomSheetNavSec
            contentNavigationPanelOrderStart: 1
        }
    }
}
