// A labelled path field with a "Browse…" button — the Qt equivalent of
// `launcher/src/filepicker.rs`'s `path_field`.
//
// Typing directly is still allowed (a path the user already knows, or one
// on a mount the picker can't reach); the button is a convenience, not
// the only way in. `FileDialog` is Qt's own, which on Linux is the XDG
// desktop portal — the same backend the egui build reaches through
// `rfd`, with no extra dependency.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import com._2ksbox.launcher

RowLayout {
    id: root

    property alias label: caption.text
    /// What the field shows. The owner writes it — usually as a binding
    /// on the model property the path lives in — and this file never
    /// does: a JS assignment from in here would *destroy* that binding
    /// (QML drops a binding on the first imperative write to its
    /// property), and the field would then keep showing the last path it
    /// happened to be given while the model moved on underneath it. That
    /// is what "New profile… still shows the previous preset" was
    /// (user-reported, 2026-09-07): the shader editor's field had been
    /// unbound by its own first edit, so the empty `presetPath` a fresh
    /// profile publishes never reached it.
    property string value
    /// A path the *user* chose — typed or picked in the dialog. The
    /// owner writes it to its model, and `value` comes back through the
    /// binding, so the data flows one way and only the model decides
    /// what the field shows.
    signal edited(string path)
    /// The same path, but only when it came from the *dialog* rather
    /// than the keyboard — for a field where choosing a file is the whole
    /// answer and not a step towards one (the disc shelf's adder acts on
    /// it at once). `edited` is emitted for it too, so a field that has
    /// no use for the distinction never has to know about this one.
    signal picked(string path)
    /// What the dialog does with the file it was given, as a function so
    /// a probe can take the same path: a real `FileDialog` belongs to the
    /// window system and cannot be opened offscreen, and everything worth
    /// checking is downstream of this line (`Main.qml`'s `pickdisc`) —
    /// the directory the next empty field's dialog opens in included.
    function acceptPath(path) { browse.remember(path); root.edited(path); root.picked(path) }
    /// The step before that: the dialog hands back a URL, not a path.
    /// QML used to strip `file://` off `url.toString()`, which leaves
    /// `[` and `]` as `%5B`/`%5D` — a disc called "Game [1996]" went on
    /// the shelf under a path that does not exist and the machine booting
    /// it would not start (user-reported, 2026-09-12). `fileUrl` is what
    /// a probe hands this in place of a dialog.
    function acceptUrl(url) { root.acceptPath(browse.localPath(url)) }
    function fileUrl(path) { return browse.fileUrl(path) }
    /// e.g. "Disc images (*.iso *.cue *.ccd *.mds)". "All files (*)" is
    /// always offered alongside: a filter that hides the file someone is
    /// looking for is worse than no filter.
    property string nameFilter: ""
    /// What the text field is actually showing. The probes read it:
    /// `value` is what the owner *meant* the field to show, and the two
    /// disagreeing is the bug this field is shaped to prevent.
    readonly property alias shownText: field.text
    /// What the dialog is actually handed, for the same probes: a filter
    /// that loses the upper-case globs on its way here hides `GAME.CUE`.
    readonly property alias dialogFilters: dialog.nameFilters
    /// Where the dialog opens when the field is still empty — the shader
    /// preset field points it at the preset collection, which is
    /// otherwise buried in a data directory nobody would navigate to.
    property string emptyDir: ""
    /// The dialog is up. A window's Esc-closes-me `Shortcut` must stand
    /// down meanwhile: on macOS the dialog is a sheet on that window, and
    /// AppKit offers a key to the window under a sheet as a key
    /// equivalent *before* the sheet itself sees it — so Esc meant for the
    /// dialog closed the window, the sheet going with it (user-reported,
    /// 2026-09-12, the disc shelf).
    readonly property bool browsing: dialog.visible

    spacing: 8

    Label {
        id: caption
        Layout.minimumWidth: 150
        Layout.alignment: Qt.AlignVCenter
    }

    TextField {
        id: field
        Layout.fillWidth: true
        text: root.value
        selectByMouse: true
        // `textEdited`, not `textChanged`: the binding above writes this
        // field too, and echoing that back would report the model's own
        // value to it as a user edit.
        onTextEdited: root.edited(text)
    }

    Button {
        text: qsTr("Browse…")
        // Where it opens is decided when it opens (`browse::browse_start`):
        // the field's own value, else `emptyDir`, else wherever the last
        // dialog — any field's — was browsing, which moves between one
        // click and the next and so cannot be a binding.
        onClicked: {
            dialog.currentFolder = browse.startUrl(root.value, root.emptyDir)
            dialog.open()
        }
    }

    Browse { id: browse }

    FileDialog {
        id: dialog
        nameFilters: root.nameFilter === ""
            ? [qsTr("All files (*)")]
            : [root.nameFilter, qsTr("All files (*)")]
        // Every glob comes in both cases (`browse::extensions`), which is
        // twice as long as anyone needs to read in the filter combo.
        options: FileDialog.HideNameFilterDetails
        onAccepted: root.acceptUrl(selectedFile)
    }
}
