// The launcher window: the machine library grid, and the four windows
// off it. `launcher/src/main.rs`'s `LauncherApp::ui`, as a view.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com._2ksbox.launcher

ApplicationWindow {
    id: root

    width: 900
    height: 560
    visible: true
    title: qsTr("2ksbox")

    // --- state ------------------------------------------------------

    MachineModel {
        id: machines
        Component.onCompleted: refresh()
    }

    // A child process has no way to push the news that it exited, so
    // this polls for it — where the egui build did the same work at the
    // top of every frame, sixty times a second, because it had a frame
    // anyway. Here the interval is stated out loud.
    Timer {
        interval: 500
        running: true
        repeat: true
        onTriggered: machines.poll()
    }

    Diag { id: diag }

    // The first-run shader offer (`src/qt/firstrun.rs`): a question over
    // the grid on the first start of a launcher with no preset
    // collection, and never again once it has been answered. The model's
    // `step` drives the dialog both ways, the way the wizard's and the
    // shader editor's `open` flags drive their windows.
    FirstRun {
        id: offer
        // Each step that has something to answer opens its own dialog;
        // neither is ever closed from here (`FirstRunDialog.qml` says
        // why at length — a MessageDialog closed programmatically emits
        // `rejected()`, and would answer its own question).
        onStepChanged: {
            if (step === "asking") {
                firstRunDialog.open()
            } else if (step === "failed" || step === "done") {
                firstRunResultDialog.open()
            } else if (step === "") {
                // Answered and finished with. A collection may have
                // landed since these two were built, and with it the
                // starter profiles: the editor cached "there is none" at
                // construction, and the grid's Shader column lists what
                // the profile library holds.
                editor.rescanPresets()
                profiles.refresh()
                machines.refresh()
            }
        }
        // The first step is set before this file's bindings exist, so it
        // never arrives as a change.
        Component.onCompleted: if (step === "asking") firstRunDialog.open()
    }

    FirstRunDialog {
        id: firstRunDialog
        offer: offer
    }

    FirstRunResultDialog {
        id: firstRunResultDialog
        offer: offer
    }

    // The download runs on its own thread; nothing else would move the
    // megabytes in the header. It stops when the download does.
    Timer {
        interval: 300
        running: offer.busy
        repeat: true
        onTriggered: offer.poll()
    }

    // --- the grid ---------------------------------------------------

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            Label {
                text: qsTr("Machines")
                font.pixelSize: 18
                font.bold: true
                Layout.fillWidth: true
            }
            // The preset download, while it runs. Here and not in the
            // dialog: it needs no answer, and a modal window with a
            // spinner in it would lock the launcher for a minute over a
            // job the user has already agreed to. Both strings are the
            // model's (`firstrun::Message`).
            RowLayout {
                spacing: 6
                visible: offer.busy
                BusyIndicator { implicitWidth: 16; implicitHeight: 16; running: visible }
                Label {
                    text: offer.headline + " " + offer.detail
                    opacity: 0.7
                }
            }
            Label {
                // A failure to start a player is a whole sentence with
                // a path in it, and this label is 320px wide: without
                // the tooltip the elided head reads as nothing having
                // happened at all.
                text: machines.status
                opacity: 0.7
                elide: Text.ElideRight
                Layout.maximumWidth: 320
                ToolTip.visible: statusHover.hovered && machines.status !== ""
                ToolTip.text: machines.status
                HoverHandler { id: statusHover }
            }
        }
    }

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // Column widths shared by the header and every row, so the two
        // cannot drift the way two separate layouts would.
        QtObject {
            id: cols
            readonly property int name: 190
            readonly property int family: 80
            readonly property int shader: 170
            readonly property int actions: 300
        }

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: 0
            // The Basic style's Frame paints only a border, so the area
            // below the last row would otherwise show whatever is behind
            // the window — black in a grab, the window colour on screen.
            background: Rectangle {
                color: palette.base
                border.color: palette.mid
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // Header row
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 32
                    color: palette.alternateBase

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 10

                        Label { text: qsTr("Name"); font.bold: true; Layout.preferredWidth: cols.name }
                        Label { text: qsTr("Family"); font.bold: true; Layout.preferredWidth: cols.family }
                        Label { text: qsTr("Shader"); font.bold: true; Layout.preferredWidth: cols.shader }
                        Label { text: qsTr("Location"); font.bold: true; Layout.fillWidth: true }
                        Item { Layout.preferredWidth: cols.actions }
                    }
                }

                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: machines
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Rectangle {
                        id: machineRow

                        // Declared `required`, so the roles arrive as
                        // real properties of this item rather than out of
                        // a context object nothing can see.
                        required property int index
                        required property string name
                        required property string family
                        required property string shader
                        required property string location
                        required property bool running

                        width: list.width
                        implicitHeight: 40
                        color: index % 2 ? palette.base : palette.alternateBase

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 10

                            Label {
                                text: machineRow.name
                                elide: Text.ElideRight
                                Layout.preferredWidth: cols.name
                            }
                            Label {
                                text: machineRow.family
                                Layout.preferredWidth: cols.family
                            }
                            Label {
                                text: machineRow.shader
                                elide: Text.ElideRight
                                Layout.preferredWidth: cols.shader
                            }
                            Label {
                                // Elides from the left: a library's paths
                                // share a long prefix, so the tail is the
                                // half that identifies the row.
                                text: machineRow.location
                                elide: Text.ElideLeft
                                opacity: 0.7
                                Layout.fillWidth: true
                                ToolTip.visible: pathHover.hovered
                                ToolTip.text: machineRow.location
                                HoverHandler { id: pathHover }
                            }

                            RowLayout {
                                spacing: 6
                                Layout.preferredWidth: cols.actions

                                Label {
                                    text: qsTr("Running")
                                    visible: machineRow.running
                                    color: palette.highlight
                                    Layout.preferredWidth: 60
                                }
                                Button {
                                    text: qsTr("Play")
                                    visible: !machineRow.running
                                    Layout.preferredWidth: 60
                                    onClicked: machines.play(machineRow.index)
                                }
                                Button {
                                    text: qsTr("Edit…")
                                    onClicked: {
                                        profiles.refresh()
                                        wizard.openEdit(machines.bundlePath(machineRow.index))
                                    }
                                }
                                Button {
                                    text: qsTr("Discs…")
                                    onClicked: {
                                        discs.openFor(machines.bundlePath(machineRow.index),
                                                      machines.discLibraryPath(),
                                                      machines.isRunning(machineRow.index))
                                        discShelfWindow.show()
                                    }
                                }
                                Button {
                                    text: qsTr("Snapshots…")
                                    onClicked: {
                                        snapshots.openFor(machines.bundlePath(machineRow.index),
                                                          machines.isRunning(machineRow.index))
                                        snapshotsWindow.show()
                                    }
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: machines.count === 0
                        horizontalAlignment: Text.AlignHCenter
                        opacity: 0.7
                        text: qsTr("No machines yet.\n%1").arg(machines.libraryDir)
                    }
                }
            }
        }

        RowLayout {
            spacing: 8
            Button {
                text: qsTr("New machine…")
                onClicked: {
                    wizard.openFresh()
                    profiles.refresh()
                }
            }
            Button {
                text: qsTr("Disc shelf…")
                onClicked: {
                    discs.openLibrary(machines.discLibraryPath())
                    discShelfWindow.show()
                }
            }
            Button {
                text: qsTr("Shader profiles…")
                onClicked: {
                    profiles.refresh()
                    shaderWindow.show()
                }
            }
            Item { Layout.fillWidth: true }
        }
    }

    // --- the secondary windows --------------------------------------

    Wizard {
        id: wizard
        // The form's own `open` flag drives the window in both
        // directions, so a `submit()` that succeeds (which clears it)
        // puts the window away wherever it was called from.
        onOpenChanged: open ? wizardWindow.show() : closeIfShown(wizardWindow)
    }
    DiscModel { id: discs }
    SnapshotModel { id: snapshots }
    ProfileModel { id: profiles }
    ShaderEditor {
        id: editor
        // Same shape as the wizard: the editor's own `open` flag drives
        // its window both ways, so a save (which clears it) puts the
        // window away wherever it was called from.
        onOpenChanged: open ? shaderEditorWindow.show() : closeIfShown(shaderEditorWindow)
    }

    /// Close a flag-driven window, unless it is already on its way out.
    ///
    /// The flag is cleared from two directions: by a button (Cancel, a
    /// successful save), and by the window's own `onVisibleChanged` when
    /// the title bar's close button hid it. The second one is already
    /// inside Qt's close — `destroy()` flips `visible` and emits the
    /// signal *before* it unregisters the modal window and hides the
    /// platform window — so calling `close()` back from there delivers a
    /// second close event to a window that is half gone: it deletes the
    /// platform window from inside the first event, and the first one,
    /// finding it null, skips the platform `setVisible(false)`. On macOS
    /// that skipped call is `endModalSession`, so the dialog was gone
    /// and the main window stayed locked behind it (user-reported,
    /// 2026-09-07). `visible` is already false at that moment, which is
    /// the tell. The `closebox` probe below counts the close events the
    /// window receives; the `qt-close` check in `scripts/test.sh` wants
    /// exactly one.
    function closeIfShown(w) {
        if (w.visible)
            w.close()
    }

    WizardWindow {
        id: wizardWindow
        wizard: wizard
        profiles: profiles
        onSaved: machines.refresh()
    }

    DiscShelfWindow {
        id: discShelfWindow
        discs: discs
        onChanged: {
            machines.refresh()
            // A disc added or renamed should show up in the guest's own
            // CDSHELF listing without restarting the machine, so every
            // *running* drive gets the new shelf file — the egui build's
            // `take_saved` loop, moved out here where the running set
            // lives.
            if (discs.takeSaved())
                machines.republishShelf()
        }
    }

    SnapshotsWindow {
        id: snapshotsWindow
        snapshots: snapshots
    }

    ShaderProfilesWindow {
        id: shaderWindow
        profiles: profiles
        editor: editor
        onChanged: machines.refresh()
    }

    ShaderEditorWindow {
        id: shaderEditorWindow
        editor: editor
        profilesDir: machines.profileDir()
        onChanged: {
            profiles.refresh()
            machines.refresh()
        }
    }

    // --- headless screenshots (see src/qt/diag.rs) -------------------

    /// The open secondary window's own QML-declared body, or null when
    /// the grid is what should be captured.
    function openWindowItem() {
        // The first-run offer is not in this list: it is a
        // `MessageDialog`, a window the platform builds, and
        // `grabToImage` only works on an item the QML engine created
        // (see the note on `grabTimer` below). Its `firstrun` probe
        // screen prints what it holds instead of photographing it.
        const windows = [wizardWindow, discShelfWindow, snapshotsWindow,
                         shaderWindow, shaderEditorWindow]
        for (const d of windows)
            if (d.visible && d.grabItem)
                return d.grabItem
        return null
    }

    /// Close events the wizard window has received — the `closebox`
    /// probe's count.
    property int closeEvents: 0
    Connections {
        target: wizardWindow
        function onClosing(close) { closeEvents++ }
    }

    Timer {
        // A screen with no shot path is a run that only *drives* the
        // window and prints what it shows — which is the half of this
        // that needs no GPU, and the only half that works on a busy one.
        running: diag.shotPath !== "" || diag.screen !== ""
        interval: diag.delayMs
        onTriggered: {
            diag.note("arming screen=" + diag.screen + " shot=" + diag.shotPath)
            switch (diag.screen) {
            case "wizard":
                wizard.openFresh(); profiles.refresh(); wizardWindow.show()
                // `LAUNCHER_QT_ARG=<win98|xp|dos|other>` exercises the one piece
                // of form behaviour a screenshot can actually prove:
                // switching family moves the memory, processor,
                // acceleration and networking defaults with it, but only
                // while nobody has chosen them. The order is the shared
                // form's own (`bundle::Family::ALL`), which is also the
                // order `familyLabels()` hands the combo box, so the two
                // cannot get out of step.
                const families = ["win98", "xp", "dos", "other"]
                // Typed *before* the family moves, because the order is
                // the bug: a text field writes the model property and
                // nothing else, so a verb that republishes the form
                // without catching it up first writes the form's stale,
                // empty name back over what was typed (user, 2026-09-08).
                wizardWindow.typeName("Typed name")
                if (families.indexOf(diag.arg) >= 0)
                    wizard.chooseFamily(families.indexOf(diag.arg))
                // What the memory field ended up showing, beside what the
                // model says it should: a spin box bounds the value it is
                // handed against the range it has at that moment, so the
                // two can disagree and nothing but a picture would say so
                // (they did: a fresh Win98 machine showed 32 MB).
                diag.note("wizard memory: shown " + wizardWindow.shownRamMb
                          + ", model " + wizard.ramMb
                          + ", range " + wizard.ramMin + ".." + wizard.ramMax)
                diag.note("wizard name: shown [" + wizardWindow.shownName
                          + "] model [" + wizard.name + "]")
                break
            case "closebox":
                // The title bar's close button on the wizard, the way the
                // window system delivers it — a close *event*, not
                // `close()`, which is the path a button takes and which
                // Qt guards against re-entry. Counts the close events the
                // window sees (`closeIfShown` says why two is the bug)
                // and asks whether a modal window is still registered.
                wizard.openFresh(); profiles.refresh(); wizardWindow.show()
                closeEvents = 0
                const modalLeft = diag.closeModalFromWindowSystem()
                diag.note("closebox: " + closeEvents + " close events, open="
                          + wizard.open + ", visible=" + wizardWindow.visible
                          + ", modal left=" + modalLeft)
                break
            case "create":
                // `LAUNCHER_QT_ARG=[<family>:]<name>` — the whole create
                // path, ending on the refreshed grid, so the run is only a
                // pass if the bundle really landed in the library. The
                // family is worth naming: a bundle written through this
                // window has to come out the same as one written by the
                // egui build or by `--wizard-new`, and DOS is the family
                // where that used to be false.
                wizard.openFresh()
                const spec = diag.arg.split(":")
                const named = spec.length > 1
                const family = named ? ["win98", "xp", "dos", "other"].indexOf(spec[0]) : 1
                wizard.chooseFamily(family < 0 ? 1 : family)
                wizard.name = named ? spec[1] : diag.arg
                wizard.existingDisk = true
                wizard.diskPath = "/dev/null"
                diag.note("submit -> " + wizard.submit() + " " + wizard.savedPath())
                machines.refresh()
                break
            case "adddisc":
                // `LAUNCHER_QT_ARG=<path>` onto the shared shelf.
                discs.openLibrary(machines.discLibraryPath())
                discs.add(diag.arg)
                diag.note("shelf now " + discs.count + " discs, status: " + discs.status)
                discShelfWindow.show()
                break
            case "pickdisc":
                // `LAUNCHER_QT_ARG=<path>` through the "Add disc"
                // field's *dialog*, not the model: a picked disc goes on
                // the shelf on its own, and the field it came through is
                // left empty (2026-09-09). Nothing that asks the model
                // can see either half.
                discs.openLibrary(machines.discLibraryPath())
                discShelfWindow.show()
                discShelfWindow.pickDisc(diag.arg)
                diag.note("pickdisc: shelf " + discs.count + ", field ["
                          + discShelfWindow.shownAdd + "], status: " + discs.status)
                break
            case "discs":
                if (diag.arg === "")
                    discs.openLibrary(machines.discLibraryPath())
                else
                    discs.openFor(diag.arg, machines.discLibraryPath(), false)
                discShelfWindow.show(); break
            case "snapshots":
                snapshots.openFor(diag.arg, false); snapshotsWindow.show(); break
            case "profiles":
                profiles.refresh(); shaderWindow.show(); break
            case "saveprofile":
                // `LAUNCHER_QT_ARG=<preset path>` — the flow a person
                // does: the profile list open, New profile…, a name, a
                // preset typed into the real field, Save, and then New
                // profile… again. Three things only a probe that drives
                // the *windows* can see, because the model is right in
                // all of them (doc 07, 2026-09-07): the list behind the
                // editor has to gain the profile that was just saved,
                // the fresh editor's preset field has to come up empty
                // rather than still showing the last one, and the name
                // typed before the preset has to survive picking it —
                // `save()` failing on `a name is required` is that one.
                profiles.refresh()
                shaderWindow.show()
                const before = profiles.count
                editor.newProfile()
                editor.name = "Probe profile"
                shaderEditorWindow.typePreset(diag.arg)
                shaderEditorWindow.clickSave()
                diag.note("saveprofile: list " + before + " -> " + profiles.count
                          + ", editor open=" + editor.open
                          + ", dir=" + shaderEditorWindow.profilesDir
                          + ", error='" + editor.error + "'")
                editor.newProfile()
                diag.note("saveprofile: fresh preset field '"
                          + shaderEditorWindow.shownPreset
                          + "', model '" + editor.presetPath + "'")
                break
            case "firstrun":
                // The offer, driven through the dialog itself rather
                // than through the model behind it: that the real
                // `MessageDialog` is up, that it is showing the model's
                // words, and that its reject button (No) both closes it
                // and is remembered — the marker the check looks for.
                // `LAUNCHER_QT_ARG=decline` presses it; anything else
                // leaves the question standing.
                diag.note("firstrun: open=" + offer.open + ", dialog=" + firstRunDialog.visible
                          + ", step=" + offer.step + ", modality=" + firstRunDialog.modality
                          + ", buttons=" + firstRunDialog.buttons)
                diag.note("firstrun text: " + firstRunDialog.text + " | "
                          + firstRunDialog.informativeText.replace(/\n/g, " "))
                // The answers go in through the *dialog's* own signals —
                // what its standard buttons deliver — so the wiring from
                // a button to a verb is checked, not bypassed. (Not
                // `reject()` / `accept()`: those are the methods that
                // both emit `rejected()`, which is the whole reason
                // these dialogs are never driven programmatically.)
                if (diag.arg === "decline") {
                    firstRunDialog.rejected()
                    // Not the dialog's own visibility: only a real press
                    // of No hides it, and this emits the signal that
                    // press delivers.
                    diag.note("firstrun declined: open=" + offer.open + ", step=" + offer.step)
                }
                if (diag.arg === "accept") {
                    // Yes, and then wait for the download to end — point
                    // `LAUNCHER_SHADERS_DIR` somewhere unwritable and it
                    // ends at once, which is the cheap way to reach the
                    // step after it without fetching 50 MB.
                    firstRunDialog.accepted()
                    diag.note("firstrun accepted: dialog=" + firstRunDialog.visible
                              + ", step=" + offer.step + ", busy=" + offer.busy)
                    firstRunSettle.start()
                    return   // `firstRunSettle` grabs when it is done
                }
                break
            case "editor":
                // `<preset.slangp>;<preview image>`
                const parts = diag.arg.split(";")
                profiles.refresh()
                shaderEditorWindow.editPreset(parts[0], parts[1] || "")
                break
            }
            grabTimer.restart()
        }
    }

    // The `firstrun accept` probe's wait: poll the download until it is
    // no longer running, then report what came up in the question's
    // place. What it proves is the half of the flow a still picture
    // cannot — that the question really closes on Yes, that the header
    // (not a modal) carries the download, and that the *result* dialog
    // then arrives with its own words and its own buttons.
    Timer {
        id: firstRunSettle
        interval: 200
        repeat: true
        onTriggered: {
            offer.poll()
            if (offer.busy)
                return
            stop()
            // Not whether the question is still up: this probe emits the
            // dialog's `accepted` rather than pressing its Yes, and only
            // a real press hides it. What it can say is what came after.
            diag.note("firstrun settled: result=" + firstRunResultDialog.visible
                      + ", step=" + offer.step
                      + ", buttons=" + firstRunResultDialog.buttons
                      + ", text=" + firstRunResultDialog.text
                      + " | " + firstRunResultDialog.informativeText.replace(/\n/g, " "))
            grabTimer.restart()
        }
    }

    // A second beat so the window just opened has been laid out and
    // rendered before the grab.
    Timer {
        id: grabTimer
        interval: diag.delayMs
        onTriggered: {
            if (diag.shotPath === "") {   // driven, not photographed
                Qt.quit()
                return
            }
            const cb = function (result) {
                diag.report(result.saveToFile(diag.shotPath))
                Qt.quit()
            }
            // `grabToImage` only works on an item the QML engine created:
            // it starts with `qmlEngine(this)` and a window's own
            // `contentItem` (and `Overlay.overlay`) are made in C++, so
            // both refuse with no warning. Hence the grab targets below
            // are always items declared in QML — and hence a *whole
            // window* headless shot, dialog frame and all, would need a
            // small C++ shim calling `QQuickWindow::grabWindow()`.
            // Documented in doc 07: it is the one thing the egui build's
            // own 150-line off-screen dump path does better.
            const target = openWindowItem() || body
            diag.note("grabbing " + target.width + "x" + target.height
                      + " -> " + target.grabToImage(cb))
        }
    }
}
