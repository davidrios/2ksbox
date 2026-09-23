// Snapshots (doc 07). A running machine goes through its monitor, a
// stopped one through `qemu-img`. The model decides which; this only
// draws it.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

// A real top-level window (see `WizardWindow.qml`).
Window {
    id: root

    // Typed, not `var` (see `ShaderProfilesWindow.qml`).
    required property SnapshotModel snapshots

    /// The item the headless screenshot path grabs (see `Main.qml`).
    property Item grabItem: body

    title: snapshots.title
    width: 800
    height: 500
    // Wide enough for the columns below at their fixed widths plus the
    // name column's floor, so a row never runs past the window's edge.
    minimumWidth: 640
    minimumHeight: 320
    flags: Qt.Dialog
    // One at a time (`WizardWindow.qml`): a secondary window blocks the
    // grid behind it, so there is never a second one.
    modality: Qt.ApplicationModal
    color: palette.window

    // Esc is Cancel, as in every other dialog on the desktop. Nothing to
    // put back here: this window keeps no flag of its own.
    Shortcut {
        sequences: [StandardKey.Cancel]
        onActivated: root.close()
    }

    /// The row whose "Restore" is armed and waiting for its confirming
    /// click. Restoring overwrites the disk's current state with the
    /// snapshot's and there is no undo, so a stray click on a row must
    /// not do it. Kept in the view because it belongs to this window's
    /// interaction, not to the machine.
    property string confirmRestore: ""

    // One set of column widths for the header and every row (user
    // report: they drifted apart on resize). Each row also holds the two
    // buttons, which the header did not, so at a width where a row no
    // longer fit the row's columns shrank and the header's stayed put.
    // Now the three trailing columns are fixed, the name column takes
    // whatever is left in both, and the header reserves the buttons'
    // room: as much as the armed "Restore" needs, so arming a row moves
    // nothing.
    readonly property int columnSpacing: 10
    readonly property int nameMinWidth: 60
    readonly property int takenWidth: 150
    readonly property int stateWidth: 80
    readonly property int actionsWidth: restoreMetrics.implicitWidth + columnSpacing + deleteMetrics.implicitWidth

    /// Where the layout put things, for the `snapshots` probe and the
    /// `qt-snapshots` check: the list box should be the one item that
    /// grows, so a short one means something under the "New snapshot" row
    /// took a share of the spare height; and the header's columns must
    /// start where the first row's do, at any window width.
    function layoutReport() {
        const first = list.itemAtIndex(0)
        return "window " + width + "x" + height
            + ", column h=" + bodyLayout.height
            + ", list y=" + list.y + " h=" + list.height
            + ", new-row y=" + newRow.y + " h=" + newRow.height
            + ", header x=[" + columnsX(header, [headerName, headerTaken, headerState, headerActions]) + "]"
            + ", row x=[" + (first ? first.columnsX() : "") + "]"
    }

    /// Each column's left edge in window coordinates, space-separated.
    function columnsX(row, columns) {
        return columns.map(c => Math.round(c.mapToItem(null, 0, 0).x)).join(" ")
    }

    // Runs only while a live job is in flight.
    Timer {
        interval: 400
        repeat: true
        running: root.visible && root.snapshots.busy
        onTriggered: root.snapshots.poll()
    }

    Rectangle {
        id: body
        anchors.fill: parent
        color: palette.window

        ColumnLayout {
            id: bodyLayout
            anchors.fill: parent
            anchors.margins: 14
            spacing: 8

            Label {
                Layout.fillWidth: true
                visible: root.snapshots.running
                wrapMode: Text.Wrap
                opacity: 0.75
                text: qsTr("The machine is running, so a snapshot also saves its RAM and CPU state.")
            }

            // A stock list with nothing drawn by hand (user decision: no
            // list box, no zebra rows, no colours of ours, only the style's
            // own look). A `ListView` of `ItemDelegate`s under a header row
            // of labels whose margins are a delegate's own padding, read off
            // an invisible one, so the columns line up under every style.
            // The two hidden buttons are how wide a row's widest pair of
            // buttons is under this style, for the reserved column.
            ItemDelegate { id: rowMetrics; visible: false }
            Button { id: restoreMetrics; visible: false; text: qsTr("Discard current state?") }
            Button { id: deleteMetrics; visible: false; text: qsTr("Delete") }

            RowLayout {
                id: header
                Layout.fillWidth: true
                Layout.leftMargin: rowMetrics.leftPadding
                Layout.rightMargin: rowMetrics.rightPadding
                spacing: root.columnSpacing
                Label {
                    id: headerName
                    text: qsTr("Name"); font.bold: true
                    Layout.fillWidth: true
                    Layout.minimumWidth: root.nameMinWidth
                }
                Label {
                    id: headerTaken
                    text: qsTr("Taken"); font.bold: true
                    Layout.minimumWidth: root.takenWidth
                    Layout.maximumWidth: root.takenWidth
                }
                Label {
                    id: headerState
                    text: qsTr("VM state"); font.bold: true
                    Layout.minimumWidth: root.stateWidth
                    Layout.maximumWidth: root.stateWidth
                }
                Item {
                    id: headerActions
                    Layout.minimumWidth: root.actionsWidth
                    Layout.maximumWidth: root.actionsWidth
                }
            }

            MenuSeparator { Layout.fillWidth: true }

            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: root.snapshots
                ScrollBar.vertical: ScrollBar {}

                delegate: ItemDelegate {
                    id: snapRow

                    required property int index
                    required property string name
                    required property string taken
                    required property string vmState
                    required property int depth
                    required property bool current

                    width: list.width

                    function columnsX() {
                        return root.columnsX(snapRow, [nameColumn, takenColumn, stateColumn, actions])
                    }

                    contentItem: RowLayout {
                        spacing: root.columnSpacing

                        // The tree: rows come in tree order (each root
                        // followed by its descendants), so the name is
                        // set in by its depth and a child carries a
                        // branch mark. The row the disk's present state
                        // descends from — where the next snapshot goes —
                        // says so beside its name.
                        RowLayout {
                            id: nameColumn
                            Layout.fillWidth: true
                            Layout.minimumWidth: root.nameMinWidth
                            spacing: 6
                            Item { id: indent; Layout.preferredWidth: snapRow.depth * 18; visible: snapRow.depth > 0 }
                            Label {
                                text: (snapRow.depth > 0 ? "└ " : "") + snapRow.name
                                elide: Text.ElideRight
                                // As wide as the name, up to what the column
                                // leaves after the indent and the tag, so the
                                // tag sits right after the name.
                                Layout.maximumWidth: nameColumn.width - (indent.visible ? indent.width + 6 : 0)
                                    - (currentTag.visible ? currentTag.width + 6 : 0)
                            }
                            Label {
                                id: currentTag
                                visible: snapRow.current
                                text: qsTr("current")
                                font.italic: true
                                opacity: 0.6
                            }
                            Item { Layout.fillWidth: true }
                        }
                        Label {
                            id: takenColumn
                            text: snapRow.taken
                            elide: Text.ElideRight
                            opacity: 0.75
                            Layout.minimumWidth: root.takenWidth
                            Layout.maximumWidth: root.takenWidth
                        }
                        Label {
                            id: stateColumn
                            text: snapRow.vmState
                            elide: Text.ElideRight
                            opacity: 0.75
                            Layout.minimumWidth: root.stateWidth
                            Layout.maximumWidth: root.stateWidth
                        }

                        // The buttons keep to the right of their column;
                        // an armed "Restore" grows into the room to their
                        // left, which the header reserves too.
                        RowLayout {
                            id: actions
                            spacing: root.columnSpacing
                            Layout.minimumWidth: root.actionsWidth
                            Layout.maximumWidth: root.actionsWidth
                            Item { Layout.fillWidth: true }
                            Button {
                                // A job in flight owns the guest's state;
                                // a second one on top of it is refused by
                                // QEMU anyway.
                                enabled: !root.snapshots.busy
                                text: root.confirmRestore === snapRow.name
                                    ? qsTr("Discard current state?")
                                    : qsTr("Restore")
                                onClicked: {
                                    if (root.confirmRestore === snapRow.name) {
                                        root.confirmRestore = ""
                                        root.snapshots.revert(snapRow.name)
                                    } else {
                                        root.confirmRestore = snapRow.name
                                    }
                                }
                            }
                            Button {
                                text: qsTr("Delete")
                                enabled: !root.snapshots.busy
                                onClicked: {
                                    root.confirmRestore = ""
                                    root.snapshots.dropSnapshot(snapRow.name)
                                }
                            }
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: root.snapshots.count === 0
                    opacity: 0.7
                    text: qsTr("No snapshots yet.")
                }
            }

            RowLayout {
                id: newRow
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("New snapshot") }
                TextField {
                    id: newName
                    Layout.fillWidth: true
                    selectByMouse: true
                    onAccepted: if (takeButton.enabled) takeButton.clicked()
                }
                Button {
                    id: takeButton
                    text: qsTr("Take snapshot")
                    enabled: newName.text.trim() !== "" && !root.snapshots.busy
                    onClicked: {
                        root.snapshots.take(newName.text)
                        newName.clear()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                // Not a filler, unlike every nested layout's default. Both
                // children are hidden until there is a status, and an empty
                // layout has no maximum, so it took half the spare height
                // from the list box, which stopped halfway down the window
                // (the `qt-snapshots` check covers it).
                Layout.fillHeight: false
                spacing: 8
                BusyIndicator {
                    running: root.snapshots.busy
                    visible: running
                    implicitWidth: 18
                    implicitHeight: 18
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.snapshots.status !== ""
                    text: root.snapshots.status
                    opacity: 0.75
                }
            }
            Label {
                Layout.fillWidth: true
                visible: root.snapshots.error !== ""
                text: root.snapshots.error
                color: "#d04040"
                wrapMode: Text.Wrap
            }
        }
    }
}
