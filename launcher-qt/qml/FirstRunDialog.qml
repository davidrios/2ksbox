// The first-run shader offer's question, as Qt's own confirmation
// dialog (`launcher_core::firstrun`, doc 07).
//
// A `MessageDialog` and not a hand-built one: it is application-modal,
// so the grid behind it is not usable until the question is answered,
// and it comes with the platform's standard buttons in the platform's
// own order — which is the reason not to lay a row of buttons out by
// hand, since which side the affirmative button sits on is a desktop
// convention and it differs across the three we ship to.
//
// Every word is the model's: `text` and `informativeText` are its
// `headline` and `detail`, and the egui build shows the same two. The
// buttons are the deliberate exception — a native dialog with
// hand-written button text is what looks wrong on every desktop at
// once — so the size and the destination are in the question itself
// rather than only on a button.
//
// **Nothing here is ever opened or closed programmatically**, and this
// is a dialog rather than *the* dialog for that reason. Calling
// `accept()` on a MessageDialog emits **`rejected()`** (measured
// 2026-09-09, on the Quick fallback), and `close()` behaves the same
// way, so a dialog whose visibility follows a model would answer its
// own question the moment the model moved. Instead each step that has
// something to answer gets a dialog of its own — this one and
// `FirstRunResultDialog.qml` — opened when that step arrives and closed
// only by the person pressing one of its buttons. The download in
// between has no dialog at all: it needs no answer, and `Main.qml`'s
// header carries it.
import QtQuick
import QtQuick.Dialogs
import com._2ksbox.launcher

MessageDialog {
    required property FirstRun offer

    title: offer.title
    modality: Qt.ApplicationModal
    text: offer.headline
    informativeText: offer.detail
    buttons: MessageDialog.Yes | MessageDialog.No

    onAccepted: offer.accept()
    onRejected: offer.decline()
}
