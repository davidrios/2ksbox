// Which window has the keyboard, for the headless probe.
//
// QML's `Window.active` cannot answer it. A window with a transient parent
// reports its parent's activity, and every secondary window here is
// transient for the launcher window, so they all read `true` whenever any
// one of them has the focus. Quick Controls' shortcut matcher asks the
// same `isActive()`, which is why two open windows' Esc shortcuts both
// matched (`qml/ShaderProfilesWindow.qml`). A key goes to the focus
// window, so the probe asks `QGuiApplication` directly, by title, since
// cxx-qt-lib has no `QWindow`.

#include <QtCore/QByteArray>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

extern "C" const char *launcher_qt_focus_window()
{
    static QByteArray text;
    QWindow *window = QGuiApplication::focusWindow();
    text = window ? window->title().toUtf8() : QByteArray("(none)");
    return text.constData();
}
