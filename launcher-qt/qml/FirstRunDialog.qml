// The first-run shader offer: the one question a launcher with no preset
// collection asks on the way up (`launcher_core::firstrun`, doc 07).
//
// Every sentence in here comes from the model — the question, both
// button labels, the finished line — because the egui build asks the
// same question and there is nowhere to write it twice. What is this
// file's own is the shape: a modal dialog over the grid, which is the
// point of asking at start-up rather than putting a third download
// button somewhere.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

Dialog {
    id: root

    required property FirstRun offer

    /// The item the headless probe grabs — see `Main.qml`.
    property Item grabItem: dialogBody

    readonly property string state_: offer.state
    readonly property bool running: state_.startsWith("running")
    readonly property bool failed: state_.startsWith("failed")
    readonly property bool done: state_.startsWith("done")

    title: offer.title
    modal: true
    closePolicy: Popup.NoAutoClose   // the question is answered, not dismissed
    anchors.centerIn: Overlay.overlay
    width: Math.min(520, parent ? parent.width - 40 : 520)

    // A download runs on its own thread; nothing else would move the
    // megabytes on screen. It stops the moment the download does.
    Timer {
        interval: 300
        running: root.running
        repeat: true
        onTriggered: root.offer.poll()
    }

    contentItem: Rectangle {
        id: dialogBody
        implicitHeight: dialogLayout.implicitHeight
        color: "transparent"

        ColumnLayout {
            id: dialogLayout
            width: parent.width
            spacing: 12

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: root.state_ === "asking"
                text: root.offer.question
            }

            RowLayout {
                spacing: 8
                visible: root.running
                BusyIndicator { implicitWidth: 18; implicitHeight: 18; running: visible }
                Label {
                    text: qsTr("Downloading shader presets… %1 MB")
                        .arg(root.state_.substring("running:".length))
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: root.failed
                color: "#d04040"
                text: qsTr("Couldn't download the shader presets: %1")
                    .arg(root.state_.substring("failed:".length))
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                visible: root.done
                text: root.state_.substring("done:".length)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }

                Button {
                    visible: root.state_ === "asking"
                    text: root.offer.cancelLabel
                    onClicked: root.offer.decline()
                }
                Button {
                    visible: root.state_ === "asking"
                    highlighted: true
                    text: root.offer.confirmLabel
                    onClicked: root.offer.accept()
                }
                Button {
                    visible: root.failed
                    text: qsTr("Try again")
                    onClicked: root.offer.retry()
                }
                Button {
                    visible: root.failed
                    text: qsTr("Close")
                    onClicked: root.offer.dismiss()
                }
                Button {
                    visible: root.done
                    highlighted: true
                    text: qsTr("OK")
                    onClicked: root.offer.dismiss()
                }
            }
        }
    }

    // No standard buttons: which ones there are depends on the step, and
    // their words are the model's.
    footer: null
}
