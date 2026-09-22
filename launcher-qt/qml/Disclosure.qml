// A section header that opens and closes what is under it.
//
// It exists because the section it was written for used a plain
// `CheckBox` as its expander, and a checkbox in front of "Emulation
// optimizations" says the wrong thing entirely: unticking it reads as
// *turning the optimizations off*, not as folding seven switches out of
// sight (user, 2026-09-06). Nothing in Quick Controls ships a
// disclosure, so this is the smallest honest one: a stock `ToolButton`
// (its background and hover are the style's own) whose content is a
// triangle that turns to point down when the section is open, a label,
// and no tick anywhere.
//
// The caller keeps the body: `visible: <id>.expanded` on whatever
// follows. One header, one binding, and the body stays where it reads.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolButton {
    id: root

    /// Whether the section under this header is showing. Not the
    /// button's own `checked`: a checked tool button is drawn filled
    /// under most styles, and a filled header is what a toggle looks
    /// like. The open state is said by the triangle and by the section
    /// being there, so this stays a plain button that flips a flag.
    property bool expanded: false

    onClicked: expanded = !expanded
    // Accessibility gets the same word the pointer does; a screen reader
    // that announced "checkbox" here would be repeating the mistake.
    Accessible.role: Accessible.Button
    Accessible.name: root.text
    Accessible.description: root.expanded ? qsTr("Expanded") : qsTr("Collapsed")

    contentItem: RowLayout {
        spacing: 6

        // Drawn, not typed: "▸" and "▾" are a font's problem on some
        // desktop and a box on the one that hasn't got them.
        Canvas {
            id: arrow
            implicitWidth: 10
            implicitHeight: 10
            Layout.alignment: Qt.AlignVCenter
            rotation: root.expanded ? 90 : 0
            Behavior on rotation { NumberAnimation { duration: 90 } }

            readonly property color tint: root.palette.windowText
            onTintChanged: requestPaint()

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = tint
                ctx.beginPath()
                ctx.moveTo(2, 0.5)
                ctx.lineTo(9, 5)
                ctx.lineTo(2, 9.5)
                ctx.closePath()
                ctx.fill()
            }
        }

        Label {
            Layout.fillWidth: true
            text: root.text
            elide: Text.ElideRight
        }
    }
}
