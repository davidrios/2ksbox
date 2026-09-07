// The title bar's close button, for the headless probe.
//
// A button in the window calls `QWindow::close()`, which marks the window
// as closing and then asks the platform to close it; the title bar's
// button goes the other way round — the platform (Cocoa's
// `windowShouldClose`, a WM_CLOSE, an X11 `WM_DELETE_WINDOW`) hands Qt a
// close *event* with no such mark. The two differ in exactly one respect
// that matters: only the second one can re-enter `close()` from a
// `visibleChanged` handler, and doing so is what left the main window
// locked behind a dismissed dialog on macOS (`qml/Main.qml`,
// `closeIfShown`). So the probe must send the event, not call the
// method — the same `QCloseEvent` `QWindowSystemInterface::handleCloseEvent`
// sends, delivered through the public `sendEvent` rather than the QPA
// private header.
//
// The window is found rather than passed: cxx-qt-lib has no `QWindow`,
// and every dialog in this program is application-modal, so the current
// modal window *is* the one whose close button was clicked.

#include <QtCore/QCoreApplication>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

extern "C" int launcher_qt_close_modal_from_window_system()
{
    QWindow *window = QGuiApplication::modalWindow();
    if (!window) {
        return -1;
    }
    QCloseEvent event;
    QCoreApplication::sendEvent(window, &event);
    return QGuiApplication::modalWindow() ? 1 : 0;
}
