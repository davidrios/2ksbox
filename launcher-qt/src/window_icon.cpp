// The application icon, handed to Qt as a picture.
//
// The egui build does this with one line (`ViewportBuilder::with_icon`);
// here it needs three lines of C++, because the icon is a `QIcon` and
// cxx-qt-lib binds `QImage` but not `QIcon` or `QGuiApplication`'s
// setter for it. Rather than teach it a whole type, this is the one
// call, taking the PNG bytes the Rust side already has embedded.
//
// It matters on X11, Windows and macOS, where a toolkit hands the window
// system a picture. It does *not* matter on Wayland, which never takes
// one: there the icon comes from the desktop entry that
// `QGuiApplication::setDesktopFileName` names, which `main.rs` also
// sets — the same pairing the egui build has (`with_app_id` + `with_icon`).

#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QImage>
#include <QtGui/QPixmap>

extern "C" void twoksbox_set_window_icon(const unsigned char *png, int len)
{
    QImage image;
    if (!image.loadFromData(png, len, "PNG")) {
        return; // a broken icon is not a reason to refuse to start
    }
    QGuiApplication::setWindowIcon(QIcon(QPixmap::fromImage(image)));
}
