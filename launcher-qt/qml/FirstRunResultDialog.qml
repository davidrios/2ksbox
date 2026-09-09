// What came of the first-run offer: the presets are installed, or the
// download failed and can be tried again (`launcher_core::firstrun`).
//
// A second `MessageDialog` rather than the question one reused, for the
// reason `FirstRunDialog.qml` gives at length: a dialog that is opened
// and closed to follow a model answers its own question, because
// `accept()` and `close()` both emit `rejected()`. Each of these is
// opened when its step arrives and closed by the person pressing a
// button, which is the only thing that closes a dialog on any desktop
// anyway.
//
// Its words are the model's, like the question's. Its buttons are the
// standard ones for the two outcomes: a failure can be retried or given
// up on, a success can only be acknowledged.
import QtQuick
import QtQuick.Dialogs
import com._2ksbox.launcher

MessageDialog {
    id: root

    required property FirstRun offer

    /// Which outcome this is showing. Read at the moment a button is
    /// pressed, which is before the model is told anything, so it is
    /// still the outcome the button belonged to.
    readonly property bool failed: offer.step === "failed"

    title: offer.title
    modality: Qt.ApplicationModal
    text: offer.headline
    informativeText: offer.detail
    buttons: root.failed ? (MessageDialog.Retry | MessageDialog.Cancel) : MessageDialog.Ok

    // Retry and OK are the accept role, Cancel the reject one — all this
    // has to know about the platform's buttons.
    onAccepted: root.failed ? offer.retry() : offer.dismiss()
    onRejected: offer.dismiss()
}
