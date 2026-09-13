// "Clone…" (doc 07): a new machine that is a whole copy of this one,
// disk included. The model decides the name offered, what is copied and
// whether the machine may be copied at all; this only draws it.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

// A real top-level window, modal — see `WizardWindow.qml`.
Window {
    id: root

    // Typed, not `var` — see `ShaderProfilesWindow.qml`.
    required property CloneModel cloner

    /// The item the headless screenshot path grabs — see `Main.qml`.
    property Item grabItem: body

    /// What the name field is showing, a way to type a new one over it
    /// the way a person does (select all, type), and the Clone button's
    /// own click — the `qt-clone` probe drives the window through these,
    /// so it sees what a person would.
    readonly property alias shownName: nameField.text
    function retypeName(text) {
        nameField.selectAll()
        nameField.remove(nameField.selectionStart, nameField.selectionEnd)
        nameField.insert(0, text)
    }
    function clickClone() { cloneButton.click() }

    title: cloner.title
    width: 560
    height: 280
    minimumWidth: 420
    minimumHeight: 240
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: palette.window

    // Closing the window is Cancel: the flag drives the window both ways
    // (`Main.qml`), so clearing it here keeps the two from disagreeing
    // after a close from the title bar. A copy that is running carries on
    // and lands in the grid by itself.
    onVisibleChanged: if (!visible && cloner.open) cloner.dismiss()

    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: !root.cloner.busy
        onActivated: root.close()
    }

    Rectangle {
        id: body
        anchors.fill: parent
        color: palette.window

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.cloner.note
                wrapMode: Text.Wrap
                opacity: 0.75
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label { text: qsTr("Name") }
                TextField {
                    id: nameField
                    Layout.fillWidth: true
                    selectByMouse: true
                    enabled: !root.cloner.busy && root.cloner.warning === ""
                    text: root.cloner.name
                    onTextChanged: if (text !== root.cloner.name) root.cloner.rename(text)
                    onAccepted: if (cloneButton.enabled) cloneButton.click()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.cloner.warning
                color: "#c88200"
                wrapMode: Text.Wrap
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.cloner.busy
                spacing: 4
                ProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: root.cloner.progress
                }
                Label {
                    text: root.cloner.progressLabel
                    opacity: 0.75
                }
            }

            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.cloner.error
                color: "#d04040"
                wrapMode: Text.Wrap
            }

            Item { Layout.fillHeight: true }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    enabled: !root.cloner.busy
                    onClicked: root.cloner.dismiss()
                }
                Button {
                    id: cloneButton
                    text: qsTr("Clone")
                    enabled: root.cloner.canSubmit
                    onClicked: root.cloner.submit()
                }
            }
        }
    }
}
