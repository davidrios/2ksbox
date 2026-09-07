// Which Quick Controls style draws the launcher, and in which colours.
//
// The report this exists for: on a Windows set to dark mode the launcher
// came up "all mixed up between dark and light" (user, 2026-09-06). The
// reason is visible in a screenshot taken here with a dark palette
// forced — the window and its labels go dark, and every *control* stays
// light, because the Quick Controls Fusion style paints its buttons,
// fields and combo boxes from its own colours and only the surfaces
// around them come from the palette. Half a theme is worse than either.
//
// So the launcher is a **light-mode application**, on every platform and
// whatever the desktop is set to: the colour scheme is requested and the
// palette is handed over to match the controls, rather than accepted
// from a system that may be dark. It is not the fashionable answer; it
// is the one that is the same everywhere, and a launcher is a window
// someone opens to press Play.
//
// `LAUNCHER_QT_SCHEME=system` gives the desktop's own palette back for
// anyone who wants to see it (and `=dark` forces the other one), which
// is also how the screenshot above was taken. The start-up log says
// which style and which colours a run actually got, because a report of
// "it came up the wrong colour" is otherwise unanswerable.
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtQuickControls2/QQuickStyle>

// A fallback, and only that. Asking for the name is what makes Qt
// resolve one -- `QT_QUICK_CONTROLS_STYLE`, then a `qtquickcontrols2.conf`,
// then the platform's own -- so a platform that has a native style has
// already named it by the time this runs and keeps it: Windows answers
// "Windows" here and macOS "macOS", which is what a user of either
// should be looking at. What is left is the platforms whose default is
// "Basic", a style with no system colours at all, and those get Fusion.
extern "C" void launcher_qt_choose_style() {
    if (QQuickStyle::name().isEmpty())
        QQuickStyle::setStyle(QStringLiteral("Fusion"));
}

// 0 = the desktop's own, 1 = light (the default), 2 = dark.
//
// Both halves are needed. `setColorScheme` is a *request* to the
// platform — Windows honours it, this checkout's Wayland session and the
// offscreen plugin ignore it — and the palette is what the controls and
// surfaces actually read. Asking and telling.
extern "C" void launcher_qt_set_scheme(int scheme) {
    if (scheme == 0) {
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Unknown);
        QGuiApplication::setPalette(QPalette());
        return;
    }
    const bool dark = scheme == 2;
    QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark
                                                       : Qt::ColorScheme::Light);
    QPalette p;
    if (dark) {
        p.setColor(QPalette::Window, QColor(0x35, 0x35, 0x35));
        p.setColor(QPalette::WindowText, Qt::white);
        p.setColor(QPalette::Base, QColor(0x2a, 0x2a, 0x2a));
        p.setColor(QPalette::AlternateBase, QColor(0x35, 0x35, 0x35));
        p.setColor(QPalette::ToolTipBase, QColor(0x35, 0x35, 0x35));
        p.setColor(QPalette::ToolTipText, Qt::white);
        p.setColor(QPalette::Text, Qt::white);
        p.setColor(QPalette::Button, QColor(0x35, 0x35, 0x35));
        p.setColor(QPalette::ButtonText, Qt::white);
        p.setColor(QPalette::Link, QColor(0x2a, 0x82, 0xda));
        p.setColor(QPalette::Highlight, QColor(0x2a, 0x82, 0xda));
        p.setColor(QPalette::HighlightedText, Qt::black);
        p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
    } else {
        // Fusion's own light values, which is what the controls are
        // drawn in whatever the palette says — so this is the palette
        // that matches them.
        p.setColor(QPalette::Window, QColor(0xef, 0xef, 0xef));
        p.setColor(QPalette::WindowText, Qt::black);
        p.setColor(QPalette::Base, Qt::white);
        p.setColor(QPalette::AlternateBase, QColor(0xf7, 0xf7, 0xf7));
        p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xdc));
        p.setColor(QPalette::ToolTipText, Qt::black);
        p.setColor(QPalette::Text, Qt::black);
        p.setColor(QPalette::Button, QColor(0xef, 0xef, 0xef));
        p.setColor(QPalette::ButtonText, Qt::black);
        p.setColor(QPalette::Link, QColor(0x00, 0x00, 0xff));
        p.setColor(QPalette::Highlight, QColor(0x30, 0x8c, 0xc6));
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
    }
    p.setColor(QPalette::BrightText, Qt::red);
    const QColor grey = dark ? QColor(0x7f, 0x7f, 0x7f) : QColor(0xbe, 0xbe, 0xbe);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, grey);
    QGuiApplication::setPalette(p);
}

// What actually took effect, for the start-up log: a style is chosen and
// a scheme is *requested*, and neither is guaranteed, so neither is
// worth assuming when a report says the window came up the wrong colour.
extern "C" const char *launcher_qt_appearance() {
    static QByteArray text;
    const char *scheme = "system";
    switch (QGuiApplication::styleHints()->colorScheme()) {
    case Qt::ColorScheme::Light: scheme = "light"; break;
    case Qt::ColorScheme::Dark: scheme = "dark"; break;
    case Qt::ColorScheme::Unknown: scheme = "system"; break;
    }
    text = QStringLiteral("style %1, scheme %2, window %3, base %4")
               .arg(QQuickStyle::name().isEmpty() ? QStringLiteral("(default)")
                                                  : QQuickStyle::name())
               .arg(QLatin1StringView(scheme))
               .arg(QGuiApplication::palette().color(QPalette::Window).name())
               .arg(QGuiApplication::palette().color(QPalette::Base).name())
               .toUtf8();
    return text.constData();
}
