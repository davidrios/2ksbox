// Which Quick Controls style draws the launcher, and in which colours.
//
// The launcher follows the desktop's light or dark mode on every platform
// (user decision). The trap is a half theme: a window whose surfaces are
// dark and whose controls are light, or the reverse.
//
// A Quick Controls style takes its palette from the platform theme
// (`QQuickTheme`, which never reads `QGuiApplication::palette()`), while
// a plain `Window` or `Rectangle` reads the application palette. So a
// palette handed to the application reaches the surfaces and never the
// controls. An earlier forced-light launcher produced exactly that mix as
// soon as a platform theme with a dark scheme was loaded: on sway, once
// `main.rs` asked for the XDG portal theme, Fusion drew dark controls on
// light windows and the text was unreadable. With nothing handed over,
// both halves read the same theme. Fusion is a whole theme in either
// scheme, and macOS's style follows the system appearance itself.
//
// On Windows the style Qt resolves by itself is "Windows", which draws
// Vista-era common controls and has the half-theme problem, so Windows
// gets FluentWinUI3 (Qt 6.8+; the cross image's 6.10 and MSYS2's 6.11
// both carry it). Its `Config.qml` picks its light or dark control set
// from `Application.styleHints.colorScheme`, the value `setColorScheme`
// below sets, so the controls and the palette cannot disagree. The
// hand-drawn lists in the QML take their zebra shade from `palette.base`
// rather than `alternateBase` because Windows' dark palette derives that
// role from the accent colour.
//
// `LAUNCHER_QT_SCHEME=light|dark` forces a scheme on any platform
// (`system` is the default), and `QT_QUICK_CONTROLS_STYLE` still names
// any style, for comparing one look against another. The start-up log
// records which style and colours a run got, since a report of "it came
// up the wrong colour" is otherwise unanswerable.
#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtQuickControls2/QQuickStyle>

// Windows gets FluentWinUI3 unless the environment names a style, and
// the rest a fallback only. Asking for the name makes Qt resolve one
// (`QT_QUICK_CONTROLS_STYLE`, then a `qtquickcontrols2.conf`, then the
// platform's own), so a platform with a native style has named it by the
// time this runs: macOS answers "macOS", which is right, and Windows
// "Windows", which is not (the header above). The platforms left default
// to "Basic", a style with no system colours at all, and get Fusion.
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
// A forced scheme needs both halves. `setColorScheme` is a request to the
// platform (Windows honours it; a sway session and the offscreen plugin
// ignore it), and the palette is what the surfaces read. The controls
// still read the theme's (see the header), so a forced scheme is for
// comparisons, not for use.
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
        // Fusion's own light values, which the controls are drawn in
        // whatever the palette says, so this palette matches them.
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

// What took effect, for the start-up log. A style is chosen and a scheme
// is requested, and neither is guaranteed, so the log records the result.
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
