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
//! - **Windows**: a `WH_KEYBOARD_LL` hook, installed for the window's whole
//!   life and asking itself whether the keyboard is ours, which takes the
//!   keys of every shortcut Windows itself acts on before the shell sees
//!   them:
//!   the two Windows keys (and so every Win+ shortcut), Tab, Esc, F4 and
//!   Space under Alt (the switcher, Alt+Esc, and the two `DefWindowProc`
//!   would turn into closing the player and opening its system menu), and
//!   Esc under Ctrl (the Start menu, Task Manager). winit then never sees
//!   them either, so the hook hands them to the guest itself; the modifier
//!   under them was never taken and reached the guest the ordinary way.
//!   Ctrl+Alt+Del and Win+L are the kernel's and no program gets them —
//!   hence the player's Ctrl+Alt+Shift+D.
//! - **macOS**: nothing. Cmd reaches the app already; Cmd+Tab would need an
//!   event tap and the Accessibility permission.
//!
//! **Ctrl+Alt+K** hands them back to the host and, pressed again, to the
//! guest again — the player drops the `Capture` and builds a new one, so
//! "off" is exactly the state before it was made. `PLAYER_KEYBOARD_CAPTURE=0`
//! starts a run with them the host's, and **`PLAYER_KEYBOARD_LOG=1`** makes
//! the Windows hook say what it is doing — a shortcut that still reaches the
//! host is one of four things, and only the hook can tell them apart: it was
//! never installed, Windows took it away, another program's hook is ahead of
//! ours, or the keyboard was not judged ours at that moment.

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
    use std::sync::{mpsc, Mutex};
    use std::thread::JoinHandle;
    use windows_sys::Win32::Foundation::{LPARAM, LRESULT, WPARAM};
    use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows_sys::Win32::System::Threading::{
        GetCurrentProcessId, GetCurrentThread, GetCurrentThreadId, SetThreadPriority,
        THREAD_PRIORITY_TIME_CRITICAL,
    };
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
        GetAsyncKeyState, VK_CONTROL, VK_ESCAPE, VK_F4, VK_LWIN, VK_RWIN, VK_SPACE, VK_TAB,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        CallNextHookEx, GetForegroundWindow, GetMessageW, GetWindowThreadProcessId, KillTimer,
        PostThreadMessageW, SetTimer, SetWindowsHookExW, UnhookWindowsHookEx, HC_ACTION, HHOOK,
        KBDLLHOOKSTRUCT, LLKHF_ALTDOWN, LLKHF_EXTENDED, MSG, WH_KEYBOARD_LL, WM_KEYDOWN, WM_QUIT,
        WM_SYSKEYDOWN, WM_TIMER,
    };

    // The hook procedure is a bare `extern "system" fn`, so what it needs
    // lives in statics. It runs on the thread that installed it, inside
    // that thread's message pump — which is why that is a thread of its own
    // and not the event loop's (2026-09-17). Windows gives a low-level hook
    // `LowLevelHooksTimeout` (300 ms to 1 s) per key, and since Windows 7 a
    // hook that misses it is removed silently and for good: one keystroke
    // during a shader compile or a slow present on the render thread, and
    // the Windows key was the host's again for the rest of the session.
    static VM: Mutex<Option<Qemu>> = Mutex::new(None);
    static WINDOW: AtomicIsize = AtomicIsize::new(0);
    /// The keys the hook took that the guest holds, as set 1 scancodes: their
    /// release is taken too, whatever the modifiers are by then (Alt let go
    /// before Tab).
    static HELD: Mutex<Vec<u32>> = Mutex::new(Vec::new());
    /// `PLAYER_KEYBOARD_LOG=1`: a line per key the hook is called for while
    /// the player is in front, and per shortcut it left to the host while it
    /// is not. Nothing else can say why a shortcut still reached the host —
    /// whether the hook is installed at all, whether it is called for that
    /// key, and whether the window in front is the one it is waiting for.
    static TRACE: AtomicBool = AtomicBool::new(false);
    /// What winit last said about the window's focus. The hook asks this
    /// *and* `GetForegroundWindow`, and one of the two saying yes is enough,
    /// because the two answer at different moments and neither alone was
    /// right (2026-09-18, the PC): Windows 11 had `SearchHost`'s CoreWindow
    /// as the foreground window when the hook was called for the Windows
    /// key, so the handle comparison said the key was somebody else's and
    /// let it through — the shell got the key the press was meant to take
    /// away from it. Asking Windows only who is in front is asking the
    /// shell, mid-shortcut, about a shortcut; winit's answer comes from the
    /// window's own `WM_SETFOCUS` / `WM_KILLFOCUS` and is the one that
    /// matches what the person is looking at.
    static FOCUSED: AtomicBool = AtomicBool::new(false);

    /// How often the hook is put back (ms). Windows removes a low-level hook
    /// that misses `LowLevelHooksTimeout` **silently**: nothing is returned,
    /// no message arrives, and `UnhookWindowsHookEx` on the dead handle still
    /// succeeds. A thread of its own at time-critical priority makes that
    /// unlikely and not impossible — this process also runs a TCG vCPU and a
    /// GPU queue — so the hook is re-armed on a timer, and the new one goes
    /// in *before* the old one comes out, which leaves no keystroke between
    /// them. A re-arm is two system calls.
    const REARM_MS: u32 = 2000;

    pub struct Hook {
        /// The hook's thread and its id, for the `WM_QUIT` that ends it.
        thread: Option<(u32, JoinHandle<()>)>,
    }

    impl Hook {
        /// The hook is installed for the capture's whole life, not for each
        /// spell of focus: the hook procedure asks who is in front itself,
        /// so nothing here depends on a focus event ever arriving. (The X11
        /// grab is the one that must follow focus, because an active grab
        /// takes the keyboard from whoever else has it.)
        pub fn new(hwnd: isize, vm: Qemu) -> Self {
            TRACE.store(
                std::env::var("PLAYER_KEYBOARD_LOG").as_deref() == Ok("1"),
                Ordering::Relaxed,
            );
            *VM.lock().unwrap_or_else(|e| e.into_inner()) = Some(vm);
            WINDOW.store(hwnd, Ordering::Relaxed);
            let mut h = Hook { thread: None };
            h.install();
            h
        }

        fn install(&mut self) {
            let (tx, rx) = mpsc::channel();
            let join = std::thread::Builder::new()
                .name("keyboard hook".into())
                .spawn(move || unsafe {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                    let mut h = arm(0);
                    // the hook call made this thread's message queue, so the
                    // id can take a WM_QUIT from here on
                    let _ = tx.send((GetCurrentThreadId(), h != 0));
                    if h == 0 {
                        return;
                    }
                    // A timer of the thread's own (no window): its WM_TIMER
                    // comes out of the same `GetMessageW` the system calls
                    // the hook from, so the re-arm costs the hook nothing and
                    // shares no state with it.
                    let timer = SetTimer(0, 0, REARM_MS, None);
                    let mut msg: MSG = std::mem::zeroed();
                    let mut said = false;
                    while GetMessageW(&mut msg, 0, 0, 0) > 0 {
                        if msg.message == WM_TIMER {
                            h = arm(h);
                            if !std::mem::replace(&mut said, true) && TRACE.load(Ordering::Relaxed) {
                                eprintln!("[keyboard] hook re-armed (and every {REARM_MS} ms after)");
                            }
                        }
                    }
                    KillTimer(0, timer);
                    UnhookWindowsHookEx(h);
                    if TRACE.load(Ordering::Relaxed) {
                        eprintln!("[keyboard] hook removed");
                    }
                });
            let Ok(join) = join else {
                eprintln!("[keyboard] no hook thread: the Windows key stays the host's");
                return;
            };
            match rx.recv() {
                Ok((id, true)) => {
                    if TRACE.load(Ordering::Relaxed) {
                        eprintln!(
                            "[keyboard] hook installed on thread {id}, window {:#x}",
                            WINDOW.load(Ordering::Relaxed)
                        );
                    }
                    self.thread = Some((id, join));
                }
                _ => {
                    let _ = join.join();
                    eprintln!(
                        "[keyboard] SetWindowsHookExW failed: the Windows key stays the host's"
                    );
                }
            }
        }

        /// The hook stays where it is; what focus decides is whether a
        /// shortcut is ours to take, and — on the way out — that the keys
        /// the guest is holding must be let go, because their release went
        /// to whoever took the focus.
        pub fn set(&mut self, focused: bool) {
            FOCUSED.store(focused, Ordering::Relaxed);
            if TRACE.load(Ordering::Relaxed) {
                eprintln!(
                    "[keyboard] focus: winit says {focused}, foreground {:#x}, our window {:#x}",
                    unsafe { GetForegroundWindow() },
                    WINDOW.load(Ordering::Relaxed),
                );
            }
            if focused {
                return;
            }
            let held = std::mem::take(&mut *HELD.lock().unwrap_or_else(|e| e.into_inner()));
            for sc in held {
                send(sc, false);
            }
        }
    }

    impl Drop for Hook {
        fn drop(&mut self) {
            if let Some((id, join)) = self.thread.take() {
                unsafe { PostThreadMessageW(id, WM_QUIT, 0, 0) };
                let _ = join.join();
            }
            self.set(false);
            *VM.lock().unwrap_or_else(|e| e.into_inner()) = None;
        }
    }

    /// Put the hook in, then take `old` out: in that order there is no moment
    /// with no hook of ours, and the new one is at the head of the chain.
    /// Returns the handle to keep — the old one, if the new one failed.
    unsafe fn arm(old: HHOOK) -> HHOOK {
        let new = SetWindowsHookExW(
            WH_KEYBOARD_LL,
            Some(hook),
            GetModuleHandleW(std::ptr::null()),
            0,
        );
        if new == 0 {
            return old;
        }
        if old != 0 {
            UnhookWindowsHookEx(old);
        }
        new
    }

    fn send(sc: u32, down: bool) {
        if let Some(vm) = *VM.lock().unwrap_or_else(|e| e.into_inner()) {
            vm.key(qemu_embed::atset1_to_qcode(sc), down);
            vm.input_flush();
        }
        let mut held = HELD.lock().unwrap_or_else(|e| e.into_inner());
        held.retain(|&h| h != sc);
        if down {
            held.push(sc);
        }
    }

    /// A key Windows would act on rather than pass to the window.
    fn shortcut(vk: u16, flags: u32) -> bool {
        let alt = flags & LLKHF_ALTDOWN != 0;
        let ctrl = unsafe { GetAsyncKeyState(VK_CONTROL as i32) } < 0;
        match vk {
            VK_LWIN | VK_RWIN => true,
            VK_TAB | VK_F4 | VK_SPACE => alt,
            VK_ESCAPE => alt || ctrl,
            _ => false,
        }
    }

    /// Is the keyboard ours to take from? Either answer is enough (see
    /// `FOCUSED`): winit's, from the window's own focus messages, or
    /// Windows', from whichever window is in front — asked of the process as
    /// well as the handle, because a window that is ours without being
    /// *that* handle is still the player having the keyboard.
    unsafe fn ours(fg: isize) -> bool {
        if FOCUSED.load(Ordering::Relaxed) || fg == WINDOW.load(Ordering::Relaxed) {
            return true;
        }
        let mut pid = 0u32;
        GetWindowThreadProcessId(fg, &mut pid);
        pid != 0 && pid == GetCurrentProcessId()
    }

    unsafe extern "system" fn hook(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
        if code == HC_ACTION as i32 {
            // Every key on the machine passes here, ours or not, so the
            // "not ours" side stays three system calls and an atomic.
            let fg = GetForegroundWindow();
            let k = &*(lparam as *const KBDLLHOOKSTRUCT);
            let down = matches!(wparam as u32, WM_KEYDOWN | WM_SYSKEYDOWN);
            if ours(fg) {
                let sc = k.scanCode | if k.flags & LLKHF_EXTENDED != 0 { 0xE000 } else { 0 };
                let held = HELD.lock().unwrap_or_else(|e| e.into_inner()).contains(&sc);
                let take = held || (down && shortcut(k.vkCode as u16, k.flags));
                if TRACE.load(Ordering::Relaxed) {
                    eprintln!(
                        "[keyboard] hook: vk {:#04x} sc {sc:#06x} flags {:#04x} {} {}",
                        k.vkCode,
                        k.flags,
                        if down { "down" } else { "up" },
                        if take { "taken" } else { "passed on" },
                    );
                }
                if take {
                    send(sc, down);
                    return 1;
                }
            } else if TRACE.load(Ordering::Relaxed) && down && shortcut(k.vkCode as u16, k.flags) {
                eprintln!(
                    "[keyboard] hook: vk {:#04x} left to the host — neither winit nor Windows \
                     says the keyboard is ours (foreground {fg:#x}, our window {:#x})",
                    k.vkCode,
                    WINDOW.load(Ordering::Relaxed),
                );
            }
        }
        CallNextHookEx(0, code, wparam, lparam)
    }
}
