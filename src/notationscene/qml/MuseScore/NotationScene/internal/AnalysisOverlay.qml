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
import MuseScore.NotationScene

//! Over the notation view: the "Analysis" switch and, while it is on, its toolbar. The drawing itself is done by
//! the notation view (it passes its input to the analysis while it is on).
Item {
    id: root

    property NavigationSection navigationSection: null
    property int navigationOrder: 0

    function closeTextBox() {
        focusTextTimer.stop()
        textBox.visible = false
        Qt.inputMethod.hide()
    }

    AnalysisToolbarModel {
        id: model

        Component.onCompleted: load()

        onTextRequested: {
            textBox.text = ""
            textBox.visible = true
            textField.currentText = ""
            // After the tap on the score is over: the score view takes the focus back on the press and release
            focusTextTimer.restart()
        }
    }

    NavigationPanel {
        id: navPanel
        name: "AnalysisToolbar"
        section: root.navigationSection
        order: root.navigationOrder
        direction: NavigationPanel.Horizontal
        accessible.name: qsTrc("notation", "Analysis")
        enabled: root.visible && model.isAvailable
    }

    // The switch: always there when a saved score is open
    FlatButton {
        id: toggleButton

        anchors.top: parent.top
        anchors.topMargin: root.width < 600 ? 4 : 8
        anchors.right: parent.right
        anchors.rightMargin: root.width < 600 ? 4 : 12

        visible: model.isAvailable
        icon: IconCode.EDIT
        // Narrow screens (phones): only the icon, to cover less of the score
        text: root.width < 600 ? "" : (model.isActive ? qsTrc("notation", "Analysis on") : qsTrc("notation", "Analysis"))
        orientation: Qt.Horizontal
        accentButton: model.isActive
        toolTipTitle: qsTrc("notation", "Analysis: write and draw over the score")

        navigation.panel: navPanel
        navigation.column: 0

        onClicked: model.toggle()
    }

    // The tools
    Rectangle {
        id: toolbar

        anchors.top: toggleButton.bottom
        anchors.topMargin: 6
        anchors.right: parent.right
        anchors.rightMargin: 12
        width: Math.min(toolsFlow.implicitWidth + 16, root.width - 24)
        height: toolsFlow.implicitHeight + 12

        visible: model.isActive
        color: ui.theme.backgroundPrimaryColor
        border.color: ui.theme.strokeColor
        border.width: 1
        radius: 6

        Flow {
            id: toolsFlow

            anchors.fill: parent
            anchors.margins: 6
            spacing: 4

            Repeater {
                model: [
                    { tool: AnalysisToolbarModel.Select, icon: IconCode.FIT_SELECTION, title: qsTrc("notation", "Select and move") },
                    { tool: AnalysisToolbarModel.Pen, icon: IconCode.BRUSH, title: qsTrc("notation", "Freehand") },
                    { tool: AnalysisToolbarModel.Line, icon: IconCode.LINE_NORMAL, title: qsTrc("notation", "Line") },
                    { tool: AnalysisToolbarModel.Arrow, icon: IconCode.LINE_ARROW_RIGHT, title: qsTrc("notation", "Arrow") },
                    { tool: AnalysisToolbarModel.Circle, icon: IconCode.CIRCLE, title: qsTrc("notation", "Circle") },
                    { tool: AnalysisToolbarModel.Text, icon: IconCode.TEXT_FRAME, title: qsTrc("notation", "Text") },
                    { tool: AnalysisToolbarModel.Eraser, icon: IconCode.DELETE_TANK, title: qsTrc("notation", "Eraser") }
                ]

                FlatButton {
                    required property var modelData
                    required property int index

                    icon: modelData.icon
                    toolTipTitle: modelData.title
                    accentButton: model.tool === modelData.tool

                    navigation.panel: navPanel
                    navigation.column: 1 + index

                    onClicked: model.setTool(modelData.tool)
                }
            }

            // Symbols: the current one, the others in the menu
            MenuButton {
                id: symbolButton

                text: model.symbol
                toolTipTitle: qsTrc("notation", "Symbol")
                accentButton: model.tool === AnalysisToolbarModel.Symbol

                navigation.panel: navPanel
                navigation.column: 10

                menuModel: model.symbols.map(function(s) { return { id: s, title: s } })

                onHandleMenuItem: function(itemId) {
                    model.setSymbol(itemId)
                }
            }

            // Colours
            Repeater {
                model: model.colors

                Rectangle {
                    required property var modelData
                    required property int index

                    width: 28
                    height: 28
                    radius: 14
                    color: modelData
                    border.width: Qt.colorEqual(model.color, modelData) ? 3 : 1
                    border.color: Qt.colorEqual(model.color, modelData) ? ui.theme.accentColor : ui.theme.strokeColor

                    MouseArea {
                        anchors.fill: parent
                        onClicked: model.setColor(parent.modelData)
                    }
                }
            }

            // Line width
            MenuButton {
                text: model.lineWidth <= 0.15 ? qsTrc("notation", "Thin")
                      : model.lineWidth >= 0.45 ? qsTrc("notation", "Thick") : qsTrc("notation", "Medium")
                toolTipTitle: qsTrc("notation", "Line width")

                navigation.panel: navPanel
                navigation.column: 20

                menuModel: [
                    { id: "0.12", title: qsTrc("notation", "Thin") },
                    { id: "0.25", title: qsTrc("notation", "Medium") },
                    { id: "0.5", title: qsTrc("notation", "Thick") }
                ]

                onHandleMenuItem: function(itemId) {
                    model.setLineWidth(Number(itemId))
                }
            }

            FlatButton {
                icon: IconCode.UNDO
                toolTipTitle: qsTrc("global", "Undo")
                enabled: model.canUndo

                navigation.panel: navPanel
                navigation.column: 21

                onClicked: model.undo()
            }

            FlatButton {
                icon: IconCode.REDO
                toolTipTitle: qsTrc("global", "Redo")
                enabled: model.canRedo

                navigation.panel: navPanel
                navigation.column: 22

                onClicked: model.redo()
            }

            FlatButton {
                visible: model.hasSelection
                icon: IconCode.DELETE_TANK
                text: qsTrc("global", "Delete")
                orientation: Qt.Horizontal

                navigation.panel: navPanel
                navigation.column: 23

                onClicked: model.deleteSelection()
            }

            // Layers: show or hide the others', clear one's own, export
            MenuButton {
                icon: IconCode.EYE_OPEN
                toolTipTitle: qsTrc("notation", "Layers and export")

                navigation.panel: navPanel
                navigation.column: 24

                menuModel: {
                    let items = []
                    for (let i = 0; i < model.layers.length; ++i) {
                        const layer = model.layers[i]
                        items.push({
                            id: "layer:" + layer.id + ":" + (layer.visible ? "0" : "1"),
                            //: An analysis layer in the menu: %1 is a person, %2 the number of objects in it
                            title: qsTrc("notation", "%1 (%2)").arg(layer.author).arg(layer.count),
                            checkable: !layer.mine,
                            checked: layer.visible,
                            enabled: !layer.mine
                        })
                    }
                    if (items.length > 0) {
                        items.push({})
                    }
                    items.push({ id: "export", title: qsTrc("notation", "Export PDF with analysis…") })
                    items.push({ id: "clear", title: qsTrc("notation", "Clear my analysis…") })
                    return items
                }

                onHandleMenuItem: function(itemId) {
                    if (itemId === "export") {
                        Qt.callLater(model.exportPdf)
                    } else if (itemId === "clear") {
                        Qt.callLater(model.clearMine)
                    } else if (itemId.startsWith("layer:")) {
                        const parts = itemId.split(":")
                        model.setLayerVisible(parts[1], parts[2] === "1")
                    }
                }
            }
        }
    }

    // The text of the text tool
    Rectangle {
        id: textBox

        property string text: ""

        anchors.top: toolbar.bottom
        anchors.topMargin: 6
        anchors.right: parent.right
        anchors.rightMargin: 12
        width: Math.min(420, root.width - 24)
        height: textRow.implicitHeight + 16

        visible: false
        color: ui.theme.backgroundPrimaryColor
        border.color: ui.theme.strokeColor
        border.width: 1
        radius: 6

        RowLayout {
            id: textRow

            anchors.fill: parent
            anchors.margins: 8
            spacing: 8

            TextInputField {
                id: textField

                Layout.fillWidth: true
                hint: qsTrc("notation", "Text on the score")

                navigation.panel: navPanel
                navigation.column: 30

                onTextChanged: function(newTextValue) {
                    textBox.text = newTextValue
                }

                onAccepted: {
                    model.addText(textBox.text)
                    root.closeTextBox()
                }
            }

            FlatButton {
                text: qsTrc("global", "OK")
                accentButton: true
                enabled: textBox.text.trim() !== ""

                navigation.panel: navPanel
                navigation.column: 31

                onClicked: {
                    model.addText(textBox.text)
                    root.closeTextBox()
                }
            }

            FlatButton {
                text: qsTrc("global", "Cancel")

                navigation.panel: navPanel
                navigation.column: 32

                onClicked: root.closeTextBox()
            }
        }
    }

    //! The focus to the text field, and the on-screen keyboard (touch screens show it only when asked)
    Timer {
        id: focusTextTimer
        interval: 250
        repeat: false
        onTriggered: {
            if (!textBox.visible) {
                return
            }
            textField.ensureActiveFocus()
            Qt.inputMethod.show()
        }
    }

    Connections {
        target: model

        function onStateChanged() {
            if (!model.isActive && textBox.visible) {
                root.closeTextBox()
            }
        }
    }
}
