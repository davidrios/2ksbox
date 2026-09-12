// The guided creation wizard (doc 07), over the shared form
// (`launcher_core::wizard::Form`). The same form doubles as "Edit
// machine" for an existing bundle, and the egui build draws the same
// fields from the same model.
//
// Every field with a *consequence* goes through an invokable
// (`chooseFamily`, `chooseRam`, …) rather than assigning the property:
// see the header of `src/qt/wizard.rs` for why.
//
// The combo boxes' labels and the file dialogs' name filters come from
// the model too (`familyLabels()`, `diskFilter()`, …) rather than being
// retyped here, so the two front ends cannot end up offering differently
// worded choices — which they did, for the family combo, until the form
// became shared.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
// FolderDialog, for the one field that names a directory (the MT-32's
// ROMs) rather than a file.
import QtQuick.Dialogs
import com._2ksbox.launcher

// A real top-level window, not an in-window popup: the launcher's
// secondary screens are separate windows the user can move and resize,
// which is what the platform already knows how to do. `Qt.Dialog` keeps
// it transient for the launcher window — the compositor stacks it above
// and gives it a dialog frame.
//
// **Modal**, and every other secondary window with it (user, 2026-09-06,
// on the first Windows build anyone else drove): they used to be
// modeless, on the theory that someone might want one open beside the
// grid, and what that actually bought was a launcher where the wizard,
// the disc shelf, the snapshots list and the profile editor can all be
// on screen at once with nothing saying which one you are answering. One
// at a time, and Esc closes it, is what every other dialog on the
// desktop does.
Window {
    id: root

    // Typed, not `var` — see `ShaderProfilesWindow.qml`.
    required property Wizard wizard
    required property ProfileModel profiles

    signal saved()

    /// The item the headless screenshot path grabs — see `Main.qml`.
    property Item grabItem: form

    /// What the memory spin box is *showing*, which is not always what
    /// the model says: a control that clamps holds the value it was
    /// given against the range it had at that moment, so a publish in
    /// the wrong order leaves the two disagreeing and only a screenshot
    /// would ever notice. The headless path prints it (`Main.qml`), and
    /// the `qt-wizard` check in `scripts/test.sh` compares it with the
    /// model's own number.
    readonly property int shownRamMb: ram.value

    /// What the name field is showing, and a way to *type* into it. A JS
    /// assignment would not do: writing the field's `text` from QML
    /// destroys the `text:` binding, and it is precisely the binding
    /// surviving a keystroke that makes the model able to wipe the name
    /// out from under the user (`src/qt/wizard.rs`'s `edit`). `insert`
    /// is what a key press does, so the `qt-wizard` check sees what the
    /// user sees.
    readonly property alias shownName: nameField.text
    function typeName(text) { nameField.insert(nameField.length, text) }

    /// What the emulation-optimization boxes are *showing*, as a mask in
    /// the model's own bit order (`optimizationsMask`), and a way to click
    /// one box and each of the three shortcuts beside them. `click()` is
    /// what a mouse click does — the toggle, `toggled` and its handler —
    /// so the `qt-wizard` check sees what the user sees, as `typeName`
    /// does for the name field.
    readonly property int optimizationBoxes: optimizations.count
    function shownOptimizationsMask() {
        let mask = 0
        for (let i = 0; i < optimizations.count; i++)
            if (optimizations.itemAt(i).box.checked)
                mask |= 1 << i
        return mask
    }
    function clickOptimization(i) { optimizations.itemAt(i).box.click() }
    function clickOptimizationShortcut(which) {
        ({ off: optAllOff, on: optAllOn, defaults: optDefaults })[which].click()
    }

    title: wizard.title
    width: 660
    height: 720
    minimumWidth: 520
    minimumHeight: 420
    flags: Qt.Dialog
    modality: Qt.ApplicationModal
    color: palette.window

    // Closing the window *is* cancelling the form: the flag drives the
    // window in both directions (`Main.qml`), so clearing it here keeps
    // the two from disagreeing after a close from the title bar.
    onVisibleChanged: if (!visible && wizard.open) wizard.open = false

    // Esc is Cancel, the way every other dialog on the desktop behaves.
    // It goes through `close()` rather than hiding the window, because
    // that is what runs `onVisibleChanged` above — the one place a
    // model's own `open` flag is put back.
    Shortcut {
        sequences: [StandardKey.Cancel]
        onActivated: root.close()
    }

    Rectangle {
        id: form
        anchors.fill: parent
        color: palette.window

        ColumnLayout {
            id: formLayout
            anchors.fill: parent
            anchors.margins: 14
            spacing: 10

            // The fields scroll, the buttons don't. The window is a
            // fixed 720 tall and opening the optimizations section is
            // enough to push "Save" past the bottom edge — a form whose
            // save button cannot be reached is worse than one that
            // scrolls. The egui build wraps the same fields in the same
            // way.
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true

                ColumnLayout {
                    id: fields
                    width: parent.width
                    spacing: 10

                GridLayout {
                    columns: 2
                    columnSpacing: 8
                    rowSpacing: 8
                    Layout.fillWidth: true

                    Label { text: qsTr("Family") }
                    ComboBox {
                        Layout.fillWidth: true
                        model: root.wizard.familyLabels()
                        currentIndex: root.wizard.family
                        onActivated: root.wizard.chooseFamily(currentIndex)
                    }

                    // Only "Other" has one (the model decides, not this
                    // file); it spans both columns so it reads as a
                    // sentence under the picker rather than a second value.
                    Label {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        visible: root.wizard.familyNote !== ""
                        text: root.wizard.familyNote
                        wrapMode: Text.Wrap
                        font.pixelSize: 11
                        opacity: 0.75
                    }

                    Label { text: qsTr("Name") }
                    TextField {
                        id: nameField
                        Layout.fillWidth: true
                        text: root.wizard.name
                        selectByMouse: true
                        onTextChanged: root.wizard.name = text
                    }
                }

                MenuSeparator { Layout.fillWidth: true }

                // --- memory -------------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: qsTr("Memory (MB)"); Layout.minimumWidth: 150 }
                    SpinBox {
                        id: ram
                        from: root.wizard.ramMin
                        to: root.wizard.ramMax
                        stepSize: 16
                        editable: true
                        value: root.wizard.ramMb
                        onValueModified: root.wizard.chooseRam(value)
                    }
                    Button {
                        text: qsTr("Default")
                        enabled: !root.wizard.ramIsDefault
                        onClicked: root.wizard.resetRam()
                    }
                    Item { Layout.fillWidth: true }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.wizard.ramNote !== ""
                    text: root.wizard.ramNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.75
                }

                // --- processor ----------------------------------------------
                // Named machines rather than a number: "how many instructions
                // per second" is not something anyone knows about their DOS
                // game, while "it wants a 486" is written on the box. This is
                // the field that decides whether an era game is playable at
                // all (doc 06), and the Qt port had no widget for it until the
                // form became shared.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: qsTr("Processor"); Layout.minimumWidth: 150 }
                    ComboBox {
                        Layout.preferredWidth: 200
                        model: root.wizard.cpuSpeedLabels()
                        currentIndex: root.wizard.cpuSpeed
                        onActivated: root.wizard.chooseCpuSpeed(currentIndex)
                    }
                    Button {
                        text: qsTr("Default")
                        enabled: !root.wizard.cpuIsDefault
                        onClicked: root.wizard.resetCpuSpeed()
                    }
                    Item { Layout.fillWidth: true }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.wizard.cpuNote !== ""
                    text: root.wizard.cpuNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.75
                }

                // --- acceleration -------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: qsTr("Acceleration"); Layout.minimumWidth: 150 }
                    ComboBox {
                        Layout.preferredWidth: 200
                        model: root.wizard.accelLabels()
                        currentIndex: root.wizard.accel
                        onActivated: root.wizard.chooseAccel(currentIndex)
                    }
                    Button {
                        text: qsTr("Default")
                        enabled: !root.wizard.accelIsDefault
                        onClicked: root.wizard.resetAccel()
                    }
                    Item { Layout.fillWidth: true }
                }
                Label {
                    Layout.fillWidth: true
                    text: root.wizard.accelNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    // The one case that is a warning rather than a note: KVM was
                    // demanded and this host hasn't got it, so the machine will
                    // refuse to start.
                    color: root.wizard.accelWarning ? "#c88200" : palette.windowText
                    opacity: root.wizard.accelWarning ? 1.0 : 0.75
                }

                // --- the display adapter ------------------------------------
                // Both the list and whether there is a choice at all come from
                // the model (`videoLabels` is a property, not an invokable,
                // because the list changes with the family); DOS is the one
                // family with none, its adapter being a fact of the era.
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.wizard.videoApplies
                    spacing: 8
                    Label { text: qsTr("Display adapter"); Layout.minimumWidth: 150 }
                    ComboBox {
                        Layout.preferredWidth: 260
                        model: root.wizard.videoLabels
                        currentIndex: root.wizard.video
                        onActivated: root.wizard.chooseVideo(currentIndex)
                    }
                    Button {
                        text: qsTr("Default")
                        enabled: !root.wizard.videoIsDefault
                        onClicked: root.wizard.resetVideo()
                    }
                    Item { Layout.fillWidth: true }
                }
                // About the machine rather than the entry selected, so it sits
                // above the notes: changing this under an installed guest is a
                // hardware change and the guest will say so.
                Label {
                    Layout.fillWidth: true
                    visible: root.wizard.videoWarning !== ""
                    text: root.wizard.videoWarning
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    color: "#c88200"
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.wizard.videoApplies
                    text: root.wizard.videoNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.75
                }

                // --- the sound card and the MIDI port (doc 20 §6) -----------
                // Two pickers rather than one: the card is what the guest
                // plays sound *effects* on and what it needs a driver for,
                // the port is what its *music* is played by, and a machine
                // of the era had both. The FM chip is in neither list — it
                // comes with the card that carried one, as it did on the
                // hardware, which is what the card's note says.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: qsTr("Sound card"); Layout.minimumWidth: 150 }
                    ComboBox {
                        Layout.preferredWidth: 260
                        model: root.wizard.soundLabels
                        currentIndex: root.wizard.sound
                        onActivated: root.wizard.chooseSound(currentIndex)
                    }
                    Button {
                        text: qsTr("Default")
                        enabled: !root.wizard.soundIsDefault
                        onClicked: root.wizard.resetSound()
                    }
                    Item { Layout.fillWidth: true }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.wizard.soundWarning !== ""
                    text: root.wizard.soundWarning
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    color: "#c88200"
                }
                Label {
                    Layout.fillWidth: true
                    text: root.wizard.soundNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.75
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: qsTr("Music (MIDI)"); Layout.minimumWidth: 150 }
                    ComboBox {
                        Layout.preferredWidth: 260
                        model: root.wizard.musicLabels
                        currentIndex: root.wizard.music
                        onActivated: root.wizard.chooseMusic(currentIndex)
                    }
                    Button {
                        text: qsTr("Default")
                        enabled: !root.wizard.musicIsDefault
                        onClicked: root.wizard.resetMusic()
                    }
                    Item { Layout.fillWidth: true }
                }
                Label {
                    Layout.fillWidth: true
                    text: root.wizard.musicNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.75
                }
                // The bank is optional — empty means the one we ship — and
                // the ROMs are not: an MT-32 machine without them is refused
                // when the form is saved, because nothing of Roland's can be
                // shipped with this program.
                PathField {
                    Layout.fillWidth: true
                    visible: root.wizard.soundfontApplies
                    label: qsTr("SoundFont (optional)")
                    nameFilter: root.wizard.soundfontFilter()
                    value: root.wizard.soundfont
                    onEdited: (path) => root.wizard.setSoundfontPath(path)
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.wizard.mt32RomsApplies
                    spacing: 8
                    Label { text: qsTr("MT-32 ROMs"); Layout.minimumWidth: 150 }
                    TextField {
                        id: mt32RomsField
                        Layout.fillWidth: true
                        text: root.wizard.mt32Roms
                        placeholderText: qsTr("a directory holding your own CM-32L control and PCM ROMs")
                        onEditingFinished: root.wizard.setMt32RomsPath(text)
                    }
                    Button {
                        // A directory, so Qt's FolderDialog rather than the
                        // PathField above: no name filter can express "a
                        // folder" (the disc shelf's "Add folder…" has the
                        // same problem).
                        text: qsTr("Browse…")
                        onClicked: mt32RomsDialog.open()
                    }
                }
                FolderDialog {
                    id: mt32RomsDialog
                    title: qsTr("Where your Roland CM-32L ROMs are")
                    currentFolder: root.wizard.mt32Roms !== "" ? "file://" + root.wizard.mt32Roms : ""
                    onAccepted: root.wizard.setMt32RomsPath(selectedFolder.toString().replace(/^file:\/\//, ""))
                }

                // --- the gamepad (M13) --------------------------------------
                // Same shape as the adapter above, and a property list for the
                // same reason: DOS is offered no USB controller, having no USB
                // stack at all, so the list changes with the family.
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.wizard.padApplies
                    spacing: 8
                    Label { text: qsTr("Gamepad"); Layout.minimumWidth: 150 }
                    ComboBox {
                        Layout.preferredWidth: 260
                        model: root.wizard.padLabels
                        currentIndex: root.wizard.pad
                        onActivated: root.wizard.choosePad(currentIndex)
                    }
                    Button {
                        text: qsTr("Default")
                        enabled: !root.wizard.padIsDefault
                        onClicked: root.wizard.resetPad()
                    }
                    Item { Layout.fillWidth: true }
                }
                // Adding or removing the USB controller is a hardware change,
                // so it is about the machine and sits above the notes.
                Label {
                    Layout.fillWidth: true
                    visible: root.wizard.padWarning !== ""
                    text: root.wizard.padWarning
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    color: "#c88200"
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.wizard.padApplies
                    text: root.wizard.padNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.75
                }

                // --- the host's 3D (ADR-013) --------------------------------
                // Stated, not chosen: the host settles which 3D stack a guest
                // gets. Empty on a DOS machine. Orange only for the software
                // Vulkan driver, the case that runs and disappoints.
                Label {
                    Layout.fillWidth: true
                    visible: root.wizard.graphicsNote !== ""
                    text: root.wizard.graphicsNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    color: root.wizard.graphicsWarning ? "#c88200" : palette.windowText
                    opacity: root.wizard.graphicsWarning ? 1.0 : 0.75
                }

                // --- networking ---------------------------------------------
                CheckBox {
                    text: qsTr("Networking")
                    checked: root.wizard.network
                    onToggled: root.wizard.chooseNetwork(checked)
                }
                Label {
                    Layout.fillWidth: true
                    text: root.wizard.networkNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.75
                }

                // --- the pointer ---------------------------------------------
                // One checkbox: does the host pointer walk into this machine
                // (the USB tablet, an absolute device) or does the window
                // take it (the PS/2 mouse, grabbed on a click). The
                // sentences are the shared form's, hotkey included.
                CheckBox {
                    text: qsTr("Seamless mouse")
                    checked: root.wizard.seamlessMouse
                    onToggled: root.wizard.chooseSeamlessMouse(checked)
                }
                Label {
                    Layout.fillWidth: true
                    text: root.wizard.seamlessMouseNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.75
                }

                // --- the Voodoo 2 -------------------------------------------
                // One checkbox: a 3dfx Voodoo 2 beside the display adapter
                // (doc 21) or not. The sentences are the shared form's.
                CheckBox {
                    text: qsTr("Emulated 3dfx Voodoo 2")
                    checked: root.wizard.voodoo2
                    onToggled: root.wizard.chooseVoodoo2(checked)
                }
                Label {
                    Layout.fillWidth: true
                    text: root.wizard.voodoo2Note
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.75
                }

                // --- emulation optimizations ---------------------------------
                // Our own QEMU fast paths (patches/qemu/README.md), one
                // checkbox each, behind a disclosure: seven switches nobody
                // needs to touch would push the fields that matter off the
                // bottom of the window. The header carries the count, so a
                // machine with one turned off says so while closed. The
                // labels, the sentences and the count all come from the
                // shared form — the egui build draws the same section from
                // the same strings.
                //
                // The header is a `Disclosure`, not a checkbox: a tick in
                // front of "Emulation optimizations" reads as the switch
                // that turns them all off, which is not what closing a
                // section does (user, 2026-09-06). egui draws this one as
                // a `CollapsingHeader` for the same reason.
                Disclosure {
                    id: optimizationsExpander
                    Layout.fillWidth: true
                    text: qsTr("Emulation optimizations — %1").arg(root.wizard.optimizationsSummary)
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 18
                    spacing: 4
                    visible: optimizationsExpander.expanded

                    Label {
                        Layout.fillWidth: true
                        text: root.wizard.optimizationsNote
                        wrapMode: Text.Wrap
                        font.pixelSize: 11
                        opacity: 0.75
                    }
                    Repeater {
                        id: optimizations
                        // Read once: both lists are fixed for the life of the
                        // build, and a binding would call across the bridge
                        // for every row on every publish.
                        readonly property var notes: root.wizard.optimizationNotes()
                        model: root.wizard.optimizationLabels()

                        ColumnLayout {
                            id: row
                            required property int index
                            required property string modelData
                            Layout.fillWidth: true
                            spacing: 0
                            readonly property alias box: optBox

                            CheckBox {
                                id: optBox
                                text: row.modelData
                                checked: (root.wizard.optimizationsMask & (1 << row.index)) !== 0
                                onToggled: root.wizard.chooseOptimization(row.index, checked)
                            }
                            Label {
                                Layout.fillWidth: true
                                Layout.leftMargin: 6
                                text: optimizations.notes[row.index]
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                opacity: 0.75
                            }
                        }
                    }
                    // The two shortcuts sit beside "All defaults" rather
                    // than replacing it: eleven switches is too many to
                    // walk through to build a control run, and the way
                    // back is not "all on" (pinned-regs ships off) but
                    // the defaults.
                    RowLayout {
                        spacing: 6
                        Button {
                            id: optAllOff
                            text: qsTr("Turn all off")
                            enabled: !root.wizard.optimizationsAllOff
                            onClicked: root.wizard.disableAllOptimizations()
                        }
                        Button {
                            id: optAllOn
                            text: qsTr("Turn all on")
                            enabled: !root.wizard.optimizationsAllOn
                            onClicked: root.wizard.enableAllOptimizations()
                        }
                        Button {
                            id: optDefaults
                            text: qsTr("All defaults")
                            enabled: !root.wizard.optimizationsAreDefault
                            onClicked: root.wizard.resetOptimizations()
                        }
                    }
                }

                MenuSeparator { Layout.fillWidth: true }

                // --- disk ---------------------------------------------------
                CheckBox {
                    visible: !root.wizard.editing
                    text: qsTr("Use an existing disk image")
                    checked: root.wizard.existingDisk
                    onToggled: root.wizard.existingDisk = checked
                }
                PathField {
                    Layout.fillWidth: true
                    visible: root.wizard.editing || root.wizard.existingDisk
                    label: qsTr("Disk path")
                    nameFilter: root.wizard.diskFilter()
                    value: root.wizard.diskPath
                    onEdited: (path) => root.wizard.diskPath = path
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: !root.wizard.editing && !root.wizard.existingDisk
                    spacing: 8
                    Label { text: qsTr("New disk size (GB)"); Layout.minimumWidth: 150 }
                    SpinBox {
                        from: 1
                        to: 128
                        value: root.wizard.diskSizeGb
                        editable: true
                        onValueModified: root.wizard.diskSizeGb = value
                    }
                    Item { Layout.fillWidth: true }
                }
                PathField {
                    Layout.fillWidth: true
                    label: qsTr("Install media (optional)")
                    nameFilter: root.wizard.mediaFilter()
                    value: root.wizard.installMedia
                    onEdited: (path) => root.wizard.installMedia = path
                }
                // A floppy in A:, and what the machine boots from — doc 06
                // lists a floppy on the Win98 machine and doc 07 lists floppy
                // images among the media the launcher handles. Two more
                // fields this port did not have.
                PathField {
                    Layout.fillWidth: true
                    label: qsTr("Floppy (optional)")
                    nameFilter: root.wizard.floppyFilter()
                    value: root.wizard.floppy
                    // Through an invokable, not the property: the boot note
                    // below depends on whether there is an image at all.
                    onEdited: (path) => root.wizard.setFloppyPath(path)
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: qsTr("Boot from"); Layout.minimumWidth: 150 }
                    ComboBox {
                        Layout.preferredWidth: 200
                        model: root.wizard.bootLabels()
                        currentIndex: root.wizard.boot
                        onActivated: root.wizard.chooseBoot(currentIndex)
                    }
                    Item { Layout.fillWidth: true }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.wizard.bootNote !== ""
                    text: root.wizard.bootNote
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    opacity: 0.75
                }

                MenuSeparator { Layout.fillWidth: true }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: qsTr("Shader profile"); Layout.minimumWidth: 150 }
                    ComboBox {
                        id: profileBox
                        Layout.fillWidth: true
                        // Index 0 is "(default)"; the profiles follow it, so a
                        // row in `profiles` is at index+1 here.
                        model: root.profiles.count + 1
                        displayText: currentIndex === 0
                            ? qsTr("(default)")
                            : root.profiles.nameAt(currentIndex - 1)
                        currentIndex: {
                            const row = root.profiles.rowOfId(root.wizard.shaderProfile)
                            return row < 0 ? 0 : row + 1
                        }
                        delegate: ItemDelegate {
                            required property int index
                            width: profileBox.width
                            text: index === 0
                                ? qsTr("(default)")
                                : root.profiles.nameAt(index - 1)
                            onClicked: {
                                profileBox.currentIndex = index
                                root.wizard.shaderProfile =
                                    index === 0 ? "" : root.profiles.idAt(index - 1)
                                profileBox.popup.close()
                            }
                        }
                    }
                }

                MenuSeparator { Layout.fillWidth: true }

                CheckBox {
                    text: qsTr("Advanced: edit machine.toml directly")
                    checked: root.wizard.advanced
                    onToggled: {
                        root.wizard.advanced = checked
                        if (checked)
                            root.wizard.fillAdvanced()
                    }
                }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 180
                    visible: root.wizard.advanced
                    TextArea {
                        text: root.wizard.advancedToml
                        font.family: "monospace"
                        selectByMouse: true
                        onTextChanged: root.wizard.advancedToml = text
                    }
                }
                }   // ColumnLayout: fields
            }       // ScrollView

            Label {
                Layout.fillWidth: true
                visible: root.wizard.error !== ""
                text: root.wizard.error
                color: "#d04040"
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    onClicked: root.wizard.open = false
                }
                Button {
                    text: root.wizard.editing ? qsTr("Save") : qsTr("Create")
                    // A failed submit leaves the window open with the
                    // reason in the error line above.
                    onClicked: if (root.wizard.submit()) root.saved()
                }
            }
        }
    }
}
