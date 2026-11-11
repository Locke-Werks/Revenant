// Images an image decoder sent: SSTV, fax, satellite APT.
//
// A strip of thumbnails, newest last, and the picked one large beside them,
// growing a line at a time while it is still arriving. Nothing picked follows
// the live image, the newest still receiving, so an operator who only watches
// sees each picture paint in. Save writes the picture as it stands as a PNG.
//
// HIDDEN UNTIL AN IMAGE ARRIVES. No decoder in the list paints anything yet,
// and an empty frame saying so would be clutter on every receiver.
// models/decoded_images.h has the rules and ui/tests/test_decoded_images.cpp
// their cases.

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Revenant

ColumnLayout {
    id: pane

    readonly property var images: engineLink.decodedImages

    // -1 follows the live image; anything else is the operator's pick.
    property int picked: -1
    readonly property int shown: picked >= 0 && picked < images.count ? picked
                                                                      : images.liveIndex
    property string saveFault: ""

    Layout.fillWidth: true
    spacing: 4
    // Whether there is anything to show, which the pane holding this reads to
    // step aside when there is not. Not visible, which is false while the pane
    // is waiting to be placed. Not "shown", which is the picked image.
    readonly property bool hasImages: images.count > 0
    visible: hasImages

    FileDialog {
        id: saveDialog

        title: "Save image as PNG"
        fileMode: FileDialog.SaveFile
        nameFilters: ["PNG image (*.png)"]
        defaultSuffix: "png"
        onAccepted: pane.saveFault = pane.images.savePng(pane.shown, selectedFile)
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 8

        Label {
            text: "images"
            color: Theme.inkDim
            font.bold: true
            font.pixelSize: Theme.sizeBody
        }

        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: big.label
            color: Theme.ink
            font.pixelSize: Theme.sizeSmall
            elide: Text.ElideRight
        }

        StatusChip {
            visible: pane.saveFault.length > 0
            label: "not saved"
            detail: pane.saveFault
        }

        RButton {
            flat: true
            checkable: true
            checked: pane.picked < 0
            text: "live"
            iconName: "live"
            ink: Theme.inkDim
            onClicked: pane.picked = -1
        }

        RButton {
            flat: true
            text: "save"
            iconName: "export"
            ink: Theme.inkDim
            enabled: pane.shown >= 0 && big.rows > 0
            onClicked: {
                pane.saveFault = ""
                saveDialog.selectedFile = pane.images.suggestedName(pane.shown)
                saveDialog.open()
            }
        }

        RButton {
            flat: true
            text: "clear"
            iconName: "eraser"
            ink: Theme.inkDim
            onClicked: {
                pane.picked = -1
                pane.images.clear()
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 6

        // The thumbnails, newest at the bottom, the shown one outlined.
        Rectangle {
            Layout.preferredWidth: 92
            Layout.fillHeight: true
            Layout.minimumHeight: 180
            radius: Theme.radius
            color: Theme.panelSolid
            border.width: 1
            border.color: Theme.border

            ListView {
                id: strip
                SmoothWheel { flick: strip }

                anchors.fill: parent
                anchors.margins: 4
                clip: true
                spacing: 4
                model: pane.images
                ScrollBar.vertical: RScrollBar {}
                onCountChanged: positionViewAtEnd()

                delegate: Rectangle {
                    id: thumb

                    required property int index
                    required property string source
                    required property string state
                    required property string time

                    width: ListView.view.width
                    height: 70
                    radius: Theme.radius
                    color: index === pane.shown ? Theme.controlHover : "transparent"
                    border.width: 1
                    border.color: index === pane.shown ? Theme.accent : Theme.border

                    Image {
                        anchors.fill: parent
                        anchors.margins: 3
                        anchors.bottomMargin: 15
                        source: thumb.source
                        sourceSize.width: 80
                        fillMode: Image.PreserveAspectFit
                        cache: false
                        asynchronous: true
                        retainWhileLoading: true
                        smooth: true
                    }

                    Label {
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: 4
                        anchors.bottomMargin: 1
                        text: thumb.time
                        color: thumb.state === "receiving" ? Theme.accent
                             : thumb.state === "aborted" ? Theme.inkWarn : Theme.inkDim
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.sizeSmall
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: pane.picked = thumb.index
                    }
                }
            }
        }

        // The shown image, scaled to fit with square pixels.
        Rectangle {
            id: big

            readonly property var entry: pane.images.revision >= 0 && pane.shown >= 0
                                         ? pane.images.get(pane.shown) : null
            readonly property string label: entry ? entry.label + "  " + entry.width + " x "
                                                    + entry.height + "  " + entry.state : ""
            readonly property int rows: entry ? entry.height : 0

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 180
            Layout.preferredHeight: 260
            radius: Theme.radius
            color: Theme.background
            border.width: 1
            border.color: Theme.border

            Image {
                anchors.fill: parent
                anchors.margins: 4
                source: big.entry ? big.entry.source : ""
                fillMode: Image.PreserveAspectFit
                cache: false
                asynchronous: true
                retainWhileLoading: true
                smooth: false
                verticalAlignment: Image.AlignTop
            }
        }
    }
}
