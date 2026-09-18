//! The host's keyboard shortcuts go to the guest while the player window
//! has focus: the Windows key opens the *guest's* Start menu, and Super+1
//! reaches the game instead of switching the host's workspace.
//!
//! winit has no keyboard grab, so this is one piece per windowing system:
//!
//! - **Wayland**: `zwp_keyboard_shortcuts_inhibit_manager_v1`. One
//!   inhibitor for the window's whole life — the compositor applies it only
//!   while the surface has keyboard focus, which is exactly the rule wanted,
//!   so focus changes need nothing from us. The compositor may still keep a
//!   binding of its own (sway's `bindsym --inhibited`, GNOME asks the user
//!   once), which is the user's way out and not ours to take.
//! - **X11**: an active `XGrabKeyboard` while focused. An active grab beats
//!   the window manager's passive ones on Super.
//! - **Windows**: the keyboard registered for raw input with
//!   `RIDEV_NOHOTKEYS`, which stops the shell acting on the two Windows keys
//!   — and so on every Win+ shortcut — while a window of this process is the
//!   foreground one. The key still arrives at the window as an ordinary
//!   `WM_KEYDOWN`, so winit delivers it and the guest gets it down the same
//!   path as every other key: this takes the key from the shell without
//!   taking it from us, and it needs no hook, no thread and no injection.
//!   What it does not cover is what Windows calls a *system* hotkey —
//!   Alt+Tab, Alt+Esc, Ctrl+Esc, Ctrl+Alt+Del, Win+L — which no program
//!   gets; hence the player's Ctrl+Alt+Shift+D for Ctrl+Alt+Del, and
//!   Alt+F4 asking before it stops the machine.
//!
//!   **This was a `WH_KEYBOARD_LL` hook until 2026-09-18**, and the hook
//!   never worked: measured on the user's PC, a low-level keyboard hook in
//!   the player is called for every key on the machine *except* while the
//!   player's own window is the foreground one, which is the only time it
//!   is wanted — zero calls, not late ones, with the hook installed and its
//!   thread answering. A hook in a third process saw those same keys, and a
//!   minimal program with a window and a hook of its own saw its own
//!   window's keys, so it is neither a Windows rule nor that window; what in
//!   this process does it was never found, and `RIDEV_NOHOTKEYS` makes the
//!   question moot. docs/00-status.md, "The first Windows host run", item 5.
//! - **macOS**: nothing. Cmd reaches the app already; Cmd+Tab would need an
//!   event tap and the Accessibility permission.
//!
//! **Ctrl+Alt+K** hands them back to the host and, pressed again, to the
//! guest again — the player drops the `Capture` and builds a new one, so
//! "off" is exactly the state before it was made. `PLAYER_KEYBOARD_CAPTURE=0`
//! starts a run with them the host's, and **`PLAYER_KEYBOARD_LOG=1`** makes
//! the Windows side say what it did: that the registration was accepted, and
//! what winit and Windows each thought about focus at every change — a
//! shortcut that still reaches the host is nearly always a window that was
//! not in front when it was pressed.

use qemu_embed::Qemu;
#[cfg(all(unix, not(target_os = "macos")))]
use winit::raw_window_handle::RawDisplayHandle;
#[cfg(not(target_os = "macos"))]
use winit::raw_window_handle::RawWindowHandle;
use winit::raw_window_handle::{HasDisplayHandle, HasWindowHandle};
use winit::window::Window;

pub enum Capture {
    #[cfg(all(unix, not(target_os = "macos")))]
    Wayland(wl::Inhibit),
    #[cfg(all(unix, not(target_os = "macos")))]
    X11(x11::Grab),
    #[cfg(windows)]
    Windows(win::NoHotkeys),
}

/// Whether a run starts with the host's shortcuts going to the guest.
pub fn on_at_start() -> bool {
    std::env::var("PLAYER_KEYBOARD_CAPTURE").as_deref() != Ok("0")
}

impl Capture {
    /// `None` on a host this cannot do anything on (the reason is printed).
    pub fn new(window: &Window, vm: Qemu) -> Option<Capture> {
        let _ = &vm; // every platform's keys reach the guest the ordinary way
        let (w, d) = match (window.window_handle(), window.display_handle()) {
            (Ok(w), Ok(d)) => (w.as_raw(), d.as_raw()),
            _ => {
                eprintln!("[keyboard] host shortcuts stay the host's: the window has no handle yet");
                return None;
            }
        };
        let made: Result<Capture, String> = match (w, d) {
            #[cfg(all(unix, not(target_os = "macos")))]
            (RawWindowHandle::Wayland(w), RawDisplayHandle::Wayland(d)) => unsafe {
                wl::Inhibit::new(d.display.as_ptr(), w.surface.as_ptr()).map(Capture::Wayland)
            },
            #[cfg(all(unix, not(target_os = "macos")))]
            (RawWindowHandle::Xlib(w), RawDisplayHandle::Xlib(d)) => match d.display {
                Some(p) => x11::Grab::new(p.as_ptr(), w.window).map(Capture::X11),
                None => Err("no Xlib display".into()),
            },
            #[cfg(windows)]
            (RawWindowHandle::Win32(w), _) => win::NoHotkeys::new(w.hwnd.get()).map(Capture::Windows),
            _ => Err("nothing to do on this windowing system".into()),
        };
        match made {
            Ok(c) => Some(c),
            Err(e) => {
                eprintln!("[keyboard] host shortcuts stay the host's: {e}");
                None
            }
        }
    }

    pub fn set_focused(&mut self, focused: bool) {
        match *self {
            #[cfg(all(unix, not(target_os = "macos")))]
            Capture::Wayland(ref mut i) => {
                let _ = focused; // the compositor follows focus itself
                i.poll();
            }
            #[cfg(all(unix, not(target_os = "macos")))]
            Capture::X11(ref mut g) => g.set(focused),
            #[cfg(windows)]
            Capture::Windows(ref mut h) => h.set(focused),
        }
    }
}

#[cfg(all(unix, not(target_os = "macos")))]
mod wl {
    use std::ffi::c_void;
    use wayland_backend::client::{Backend, ObjectId};
    use wayland_client::globals::{registry_queue_init, GlobalListContents};
    use wayland_client::protocol::{wl_registry, wl_seat, wl_surface};
    use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle};
    use wayland_protocols::wp::keyboard_shortcuts_inhibit::zv1::client::{
        zwp_keyboard_shortcuts_inhibit_manager_v1::ZwpKeyboardShortcutsInhibitManagerV1 as Manager,
        zwp_keyboard_shortcuts_inhibitor_v1::{self as inhibitor, ZwpKeyboardShortcutsInhibitorV1 as Inhibitor},
    };

    #[derive(Default)]
    pub struct State {
        said: bool,
    }

    /// The inhibitor, on a queue of our own over winit's connection: the
    /// surface is winit's, the protocol objects are ours.
    pub struct Inhibit {
        conn: Connection,
        queue: EventQueue<State>,
        state: State,
        inhibitor: Inhibitor,
    }

    impl Inhibit {
        /// # Safety
        /// `display` and `surface` are the live `wl_display` / `wl_surface`
        /// of a window that outlives the returned value.
        pub unsafe fn new(display: *mut c_void, surface: *mut c_void) -> Result<Self, String> {
            let conn = Connection::from_backend(Backend::from_foreign_display(display.cast()));
            let (globals, queue) = registry_queue_init::<State>(&conn).map_err(|e| e.to_string())?;
            let qh = queue.handle();
            let manager: Manager = globals
                .bind(&qh, 1..=1, ())
                .map_err(|_| "the compositor offers no keyboard-shortcuts-inhibit protocol".to_string())?;
            let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=1, ()).map_err(|e| format!("no seat: {e}"))?;
            let id = ObjectId::from_ptr(wl_surface::WlSurface::interface(), surface.cast())
                .map_err(|e| format!("window surface: {e}"))?;
            let surface = wl_surface::WlSurface::from_id(&conn, id).map_err(|e| format!("window surface: {e}"))?;
            let inhibitor = manager.inhibit_shortcuts(&surface, &seat, &qh, ());
            conn.flush().map_err(|e| e.to_string())?;
            Ok(Inhibit { conn, queue, state: State::default(), inhibitor })
        }

        /// Handle what the compositor said: libwayland files the events
        /// under our queue when winit reads the socket.
        pub fn poll(&mut self) {
            let _ = self.queue.dispatch_pending(&mut self.state);
        }
    }

    impl Drop for Inhibit {
        fn drop(&mut self) {
            self.inhibitor.destroy();
            let _ = self.conn.flush();
        }
    }

    impl Dispatch<Inhibitor, ()> for State {
        fn event(state: &mut Self, _: &Inhibitor, event: inhibitor::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {
            if let inhibitor::Event::Active = event {
                if !state.said {
                    state.said = true;
                    eprintln!("[keyboard] the compositor passes its shortcuts to the guest while the window has focus");
                }
            }
        }
    }

    impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
        fn event(
            _: &mut Self,
            _: &wl_registry::WlRegistry,
            _: wl_registry::Event,
            _: &GlobalListContents,
            _: &Connection,
            _: &QueueHandle<Self>,
        ) {
        }
    }

    impl Dispatch<wl_seat::WlSeat, ()> for State {
        fn event(_: &mut Self, _: &wl_seat::WlSeat, _: wl_seat::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}
    }

    impl Dispatch<Manager, ()> for State {
        fn event(_: &mut Self, _: &Manager, _: <Manager as Proxy>::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}
    }
}

#[cfg(all(unix, not(target_os = "macos")))]
mod x11 {
    use std::ffi::{c_ulong, c_void};
    use x11_dl::xlib;

    pub struct Grab {
        xlib: xlib::Xlib,
        display: *mut xlib::Display,
        window: c_ulong,
        held: bool,
        said: bool,
    }

    impl Grab {
        pub fn new(display: *mut c_void, window: c_ulong) -> Result<Self, String> {
            let xlib = xlib::Xlib::open().map_err(|e| e.to_string())?;
            Ok(Grab { xlib, display: display.cast(), window, held: false, said: false })
        }

        pub fn set(&mut self, on: bool) {
            unsafe {
                if on && !self.held {
                    let r = (self.xlib.XGrabKeyboard)(
                        self.display,
                        self.window,
                        xlib::True,
                        xlib::GrabModeAsync,
                        xlib::GrabModeAsync,
                        xlib::CurrentTime,
                    );
                    self.held = r == xlib::GrabSuccess;
                    // AlreadyGrabbed: the window manager was mid-grab (an
                    // Alt+Tab) when focus arrived; the next focus tries again
                    if !self.held && !self.said {
                        self.said = true;
                        eprintln!("[keyboard] XGrabKeyboard refused ({r}): host shortcuts stay the host's this time");
                    }
                } else if !on && self.held {
                    (self.xlib.XUngrabKeyboard)(self.display, xlib::CurrentTime);
                    self.held = false;
                }
                (self.xlib.XFlush)(self.display);
            }
        }
    }

    impl Drop for Grab {
        fn drop(&mut self) {
            self.set(false);
        }
    }
}

#[cfg(windows)]
mod win {
    use windows_sys::Win32::Foundation::GetLastError;
    use windows_sys::Win32::UI::Input::{RegisterRawInputDevices, RAWINPUTDEVICE};
    use windows_sys::Win32::UI::WindowsAndMessaging::GetForegroundWindow;

    /// The HID usage a PC keyboard is: Generic Desktop page, Keyboard.
    const USAGE_PAGE_GENERIC: u16 = 0x01;
    const USAGE_KEYBOARD: u16 = 0x06;
    /// `RIDEV_NOHOTKEYS`: while a window of this process is the foreground
    /// one, the shell does not act on the keyboard's hotkeys.
    const RIDEV_NOHOTKEYS: u32 = 0x0000_0200;
    /// `RIDEV_REMOVE`: give the usage back. It wants a null target window.
    const RIDEV_REMOVE: u32 = 0x0000_0001;

    /// The keyboard registered for raw input with `RIDEV_NOHOTKEYS`.
    ///
    /// Nothing here follows focus: the flag is a property of *being the
    /// foreground window*, which is the rule this wants, applied by the
    /// window manager rather than by us. The key still arrives at the
    /// window as an ordinary `WM_KEYDOWN`, so winit delivers it and the
    /// guest gets it down the same path as every other key — this takes
    /// the key away from the shell without taking it away from us.
    pub struct NoHotkeys {
        trace: bool,
        window: isize,
    }

    impl NoHotkeys {
        pub fn new(hwnd: isize) -> Result<Self, String> {
            register(RIDEV_NOHOTKEYS, hwnd)?;
            let trace = std::env::var("PLAYER_KEYBOARD_LOG").as_deref() == Ok("1");
            if trace {
                eprintln!(
                    "[keyboard] the Windows keys are the guest's while window {hwnd:#x} is in front"
                );
            }
            Ok(NoHotkeys { trace, window: hwnd })
        }

        /// Focus changes nothing here; the line is worth printing because a
        /// shortcut that still reached the host is nearly always a window
        /// that was not in front when it was pressed.
        pub fn set(&mut self, focused: bool) {
            if self.trace {
                eprintln!(
                    "[keyboard] focus: winit says {focused}, foreground {:#x}, our window {:#x}",
                    unsafe { GetForegroundWindow() },
                    self.window,
                );
            }
        }
    }

    impl Drop for NoHotkeys {
        fn drop(&mut self) {
            let _ = register(RIDEV_REMOVE, 0);
        }
    }

    /// Register (or unregister) the keyboard usage for this process.
    ///
    /// It replaces winit's own registration of that usage, which winit made
    /// for `DeviceEvent::Key`; the player reads only `DeviceEvent::MouseMotion`,
    /// a different usage, and that one is left alone.
    fn register(flags: u32, hwnd: isize) -> Result<(), String> {
        let rid = RAWINPUTDEVICE {
            usUsagePage: USAGE_PAGE_GENERIC,
            usUsage: USAGE_KEYBOARD,
            dwFlags: flags,
            hwndTarget: hwnd,
        };
        let ok = unsafe {
            RegisterRawInputDevices(&rid, 1, std::mem::size_of::<RAWINPUTDEVICE>() as u32)
        };
        if ok == 0 {
            return Err(format!("RegisterRawInputDevices failed ({})", unsafe {
                GetLastError()
            }));
        }
        Ok(())
    }
}
