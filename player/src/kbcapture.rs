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
//! - **Windows**: a `WH_KEYBOARD_LL` hook while focused that takes the two
//!   Windows keys before the shell sees them. winit then never sees them
//!   either, so the hook hands them to the guest itself. Ctrl+Alt+Del and
//!   Win+L are the kernel's and no program gets them — hence the player's
//!   Ctrl+Alt+Shift+D.
//! - **macOS**: nothing. Cmd reaches the app already; Cmd+Tab would need an
//!   event tap and the Accessibility permission.
//!
//! **Ctrl+Alt+K** hands them back to the host and, pressed again, to the
//! guest again — the player drops the `Capture` and builds a new one, so
//! "off" is exactly the state before it was made. `PLAYER_KEYBOARD_CAPTURE=0`
//! starts a run with them the host's.

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
    Windows(win::Hook),
}

/// Whether a run starts with the host's shortcuts going to the guest.
pub fn on_at_start() -> bool {
    std::env::var("PLAYER_KEYBOARD_CAPTURE").as_deref() != Ok("0")
}

impl Capture {
    /// `None` on a host this cannot do anything on (the reason is printed).
    pub fn new(window: &Window, vm: Qemu) -> Option<Capture> {
        let _ = &vm; // only the Windows hook injects keys itself
        let w = window.window_handle().ok()?.as_raw();
        let d = window.display_handle().ok()?.as_raw();
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
            (RawWindowHandle::Win32(w), _) => Ok(Capture::Windows(win::Hook::new(w.hwnd.get(), vm))),
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
    use qemu_embed::Qemu;
    use std::sync::atomic::{AtomicBool, AtomicIsize, Ordering};
    use std::sync::Mutex;
    use windows_sys::Win32::Foundation::{LPARAM, LRESULT, WPARAM};
    use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{VK_LWIN, VK_RWIN};
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        CallNextHookEx, GetForegroundWindow, SetWindowsHookExW, UnhookWindowsHookEx, HC_ACTION, HHOOK,
        KBDLLHOOKSTRUCT, WH_KEYBOARD_LL, WM_KEYDOWN, WM_SYSKEYDOWN,
    };

    // The hook procedure is a bare `extern "system" fn`, so what it needs
    // lives in statics. It runs on the thread that installed it — the event
    // loop's, whose message pump is what calls it.
    static VM: Mutex<Option<Qemu>> = Mutex::new(None);
    static WINDOW: AtomicIsize = AtomicIsize::new(0);
    /// The left and right Windows key as the guest last saw them.
    static HELD: [AtomicBool; 2] = [AtomicBool::new(false), AtomicBool::new(false)];
    const SCANCODES: [u32; 2] = [0xE05B, 0xE05C];

    pub struct Hook {
        hook: HHOOK,
    }

    impl Hook {
        pub fn new(hwnd: isize, vm: Qemu) -> Self {
            *VM.lock().unwrap_or_else(|e| e.into_inner()) = Some(vm);
            WINDOW.store(hwnd, Ordering::Relaxed);
            Hook { hook: 0 }
        }

        pub fn set(&mut self, on: bool) {
            if on && self.hook == 0 {
                self.hook = unsafe { SetWindowsHookExW(WH_KEYBOARD_LL, Some(hook), GetModuleHandleW(std::ptr::null()), 0) };
                if self.hook == 0 {
                    eprintln!("[keyboard] SetWindowsHookExW failed: the Windows key stays the host's");
                }
            } else if !on && self.hook != 0 {
                unsafe { UnhookWindowsHookEx(self.hook) };
                self.hook = 0;
                // the release went to whoever took focus: let go in the guest
                for i in 0..2 {
                    if HELD[i].load(Ordering::Relaxed) {
                        send(i, false);
                    }
                }
            }
        }
    }

    impl Drop for Hook {
        fn drop(&mut self) {
            self.set(false);
            *VM.lock().unwrap_or_else(|e| e.into_inner()) = None;
        }
    }

    fn send(i: usize, down: bool) {
        if let Some(vm) = *VM.lock().unwrap_or_else(|e| e.into_inner()) {
            vm.key(qemu_embed::atset1_to_qcode(SCANCODES[i]), down);
            vm.input_flush();
        }
        HELD[i].store(down, Ordering::Relaxed);
    }

    unsafe extern "system" fn hook(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
        // installed only while focused, and checked again: focus moves
        // before the event that says so reaches us
        if code == HC_ACTION as i32 && GetForegroundWindow() == WINDOW.load(Ordering::Relaxed) {
            let k = &*(lparam as *const KBDLLHOOKSTRUCT);
            let which = match k.vkCode as u16 {
                VK_LWIN => Some(0),
                VK_RWIN => Some(1),
                _ => None,
            };
            if let Some(i) = which {
                send(i, matches!(wparam as u32, WM_KEYDOWN | WM_SYSKEYDOWN));
                return 1;
            }
        }
        CallNextHookEx(0, code, wparam, lparam)
    }
}
