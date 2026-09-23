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
// So the launcher was made a **light-mode application**, on every
// platform and whatever the desktop is set to: the colour scheme was
// requested and a palette handed over to match the controls, rather
// than accepted from a system that may be dark.
//
// **Windows follows the desktop since 2026-09-22** (user decision), on
// Qt's own Windows 11 style. The style Qt resolves there by itself is
// "Windows", which draws Vista-era common controls, and it is the one
// with the half-theme problem. FluentWinUI3 (Qt 6.8+, and both the cross
// image's 6.10 and MSYS2's 6.11 carry it) is a whole theme either way:
// its `Config.qml` picks its light or dark control set from
// `Application.styleHints.colorScheme`, the same thing `setColorScheme`
// below sets, so the controls and the palette cannot disagree. The
// hand-drawn lists in the QML take their zebra shade from `palette.base`
// rather than `alternateBase` for the same reason: Windows' dark palette
// derives that role from the accent colour.
//
// **And so does everywhere else since 2026-09-23** (user decision), once
// the half theme was understood. A Quick Controls style takes its
// palette from the *platform theme* (`QQuickTheme`, which never reads
// `QGuiApplication::palette()`), while a plain `Window` or `Rectangle`
// reads the application palette — so a palette handed to the application
// reaches the surfaces and never the controls, and forcing light on a
// dark desktop *made* the mix it was meant to prevent, the moment a
// platform theme with a dark scheme was in the process. On this
// checkout's sway session that was the day `main.rs` asked for the XDG
// portal theme (for the desktop's file dialogs): its scheme is the
// portal's, Fusion drew dark controls on the forced-light windows, and
// the text was unreadable. With nothing handed over both halves read the
// same theme: Fusion is a whole theme in either scheme, and macOS's style
// follows the system appearance itself.
//
// `LAUNCHER_QT_SCHEME=light|dark` forces a scheme on any platform (`system`
// is the default), and `QT_QUICK_CONTROLS_STYLE` still names any style —
// which is how one look is compared against another. The start-up log
// says which style and which colours a run actually got, because a
// report of "it came up the wrong colour" is otherwise unanswerable.
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtQuickControls2/QQuickStyle>

// Windows gets FluentWinUI3 unless the environment names a style, and
// the rest a fallback only. Asking for the name is what makes Qt
// resolve one -- `QT_QUICK_CONTROLS_STYLE`, then a `qtquickcontrols2.conf`,
// then the platform's own -- so a platform that has a native style has
// already named it by the time this runs: macOS answers "macOS", which
// is what its user should be looking at, and Windows "Windows", which
// is not (the header above). What is left is the platforms whose default
// is "Basic", a style with no system colours at all, and those get Fusion.
extern "C" void launcher_qt_choose_style() {
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("FluentWinUI3"));
        return;
    }
#endif
    if (QQuickStyle::name().isEmpty())
        QQuickStyle::setStyle(QStringLiteral("Fusion"));
}

// 0 = the desktop's own (the default), 1 = light, 2 = dark.
//
// For a forced scheme both halves are needed. `setColorScheme` is a
// *request* to the platform — Windows honours it, this checkout's
// Wayland session and the offscreen plugin ignore it — and the palette
// is what the surfaces read (the controls read the theme's, the header:
// a forced scheme is a comparison, not a look).
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
