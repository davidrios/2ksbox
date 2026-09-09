//! Reading a host gamepad (M13 step 0, `docs/tracks/m13-gamepads.md`).
//!
//! This is the *whole* host end and nothing else: enumerate what is
//! plugged in, notice what is unplugged, turn whatever the platform calls
//! its buttons into the abstract [`Control`]s the `gamepad` crate
//! defines, and shape the analog readings. What those controls then
//! *mean* for a machine is not decided here — that is `launcher-core`'s
//! (ADR-014), and reaches the player as arguments.
//!
//! Two sources behind one interface:
//!
//! * [`Gilrs`] — real hardware, through one crate that covers evdev on
//!   Linux, XInput and DirectInput on Windows and IOKit on macOS.
//! * [`Script`] — `PLAYER_PAD_SCRIPT`, a recorded sequence replayed
//!   against the guest's own frame counter.
//!
//! The second is not a convenience. No machine that runs
//! `scripts/test.sh` has a controller plugged into it, so without a
//! synthetic pad every check in this track would need a person and a
//! device, and the project's testing policy — integration and end-to-end
//! only, and wired into the suite so it guards against regressions —
//! could not be met at all. It is the piece the rest of M13 is built on,
//! which is why step 0 exists.

use gamepad::{Control, Shaping};
use std::collections::BTreeMap;

/// One control moved. `value` is the shaped reading: -1.0..=1.0 for an
/// axis, 0.0 or 1.0 for a button.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Event {
    pub control: Control,
    pub value: f32,
}

/// Where readings come from. Both sources are polled once per published
/// guest frame from the UI thread — see [`Pads::poll`].
trait Source {
    /// Everything that changed since the last call. `frame` is the
    /// guest's published frame count, which only the script uses.
    fn poll(&mut self, frame: u64, out: &mut Vec<Event>);
    fn describe(&self) -> String;
}

// --- the synthetic pad ----------------------------------------------

/// `PLAYER_PAD_SCRIPT="30:lx=1.0,45:south=1,51:south=0"` — set `control`
/// to `value` at guest frame `n`.
///
/// Frame numbers, not milliseconds, for the same reason `PLAYER_KEYS`
/// uses them: a guest under TCG runs at whatever rate the host manages
/// that minute, and a script written in wall-clock time would land in a
/// different place in the guest's execution on every run. Against the
/// frame counter it lands in the same place every time.
///
/// A button is `0` or `1`; an axis takes any value in -1.0..=1.0 and is
/// shaped on the way out, so a script says what the stick *is* and the
/// deadzone still applies — a script can therefore prove the deadzone
/// works, which is most of what step 0 has to prove.
struct Script {
    /// (frame, control, raw value), sorted by frame.
    steps: Vec<(u64, Control, f32)>,
    next: usize,
}

impl Script {
    fn from_env() -> Option<Result<Script, String>> {
        let spec = std::env::var("PLAYER_PAD_SCRIPT").ok()?;
        Some(Script::parse(&spec))
    }

    fn parse(spec: &str) -> Result<Script, String> {
        let mut steps = Vec::new();
        for item in spec.split(',') {
            let item = item.trim();
            if item.is_empty() {
                continue;
            }
            let (frame, rest) = item
                .split_once(':')
                .ok_or_else(|| format!("{item:?}: expected <frame>:<control>=<value>"))?;
            let (name, value) = rest
                .split_once('=')
                .ok_or_else(|| format!("{item:?}: expected <frame>:<control>=<value>"))?;
            let frame: u64 = frame
                .trim()
                .parse()
                .map_err(|_| format!("{item:?}: {:?} is not a frame number", frame.trim()))?;
            let control = Control::from_name(name.trim())
                .ok_or_else(|| format!("{item:?}: no control called {:?}", name.trim()))?;
            let value: f32 = value
                .trim()
                .parse()
                .map_err(|_| format!("{item:?}: {:?} is not a number", value.trim()))?;
            if !(-1.0..=1.0).contains(&value) {
                return Err(format!("{item:?}: {value} is outside -1.0..=1.0"));
            }
            if !control.is_axis() && value != 0.0 && value != 1.0 {
                return Err(format!(
                    "{item:?}: {} is a button, so its value must be 0 or 1",
                    control.name()
                ));
            }
            steps.push((frame, control, value));
        }
        if steps.is_empty() {
            return Err("PLAYER_PAD_SCRIPT is empty".into());
        }
        steps.sort_by_key(|&(f, c, _)| (f, c));
        Ok(Script { steps, next: 0 })
    }
}

impl Source for Script {
    fn poll(&mut self, frame: u64, out: &mut Vec<Event>) {
        while self.next < self.steps.len() && self.steps[self.next].0 <= frame {
            let (_, control, value) = self.steps[self.next];
            out.push(Event { control, value });
            self.next += 1;
        }
    }

    fn describe(&self) -> String {
        format!("scripted pad, {} step(s)", self.steps.len())
    }
}

// --- real hardware ---------------------------------------------------

#[cfg(feature = "gilrs")]
mod hardware {
    use super::{Event, Source};
    use gamepad::Control;

    pub struct Gilrs {
        inner: gilrs::Gilrs,
    }

    impl Gilrs {
        pub fn new() -> Result<Gilrs, String> {
            let inner = gilrs::Gilrs::new().map_err(|e| e.to_string())?;
            Ok(Gilrs { inner })
        }

        pub fn pads(&self) -> Vec<String> {
            self.inner
                .gamepads()
                .map(|(_, g)| g.name().to_string())
                .collect()
        }

        /// gilrs's own button/axis names to ours. The face buttons map by
        /// *position*: gilrs already normalizes South/East/West/North for
        /// this exact reason, so an Xbox pad and a DualShock give the same
        /// `Control` for the same thumb.
        fn button(b: gilrs::Button) -> Option<Control> {
            use gilrs::Button as B;
            Some(match b {
                B::South => Control::South,
                B::East => Control::East,
                B::West => Control::West,
                B::North => Control::North,
                B::DPadUp => Control::DpadUp,
                B::DPadDown => Control::DpadDown,
                B::DPadLeft => Control::DpadLeft,
                B::DPadRight => Control::DpadRight,
                B::LeftTrigger => Control::LeftShoulder,
                B::RightTrigger => Control::RightShoulder,
                B::LeftTrigger2 => Control::LeftTrigger,
                B::RightTrigger2 => Control::RightTrigger,
                B::Select => Control::Select,
                B::Start => Control::Start,
                B::LeftThumb => Control::LeftStickPress,
                B::RightThumb => Control::RightStickPress,
                // Mode/C/Z and anything else this pad invents: a control
                // we have no name for, and silently ignoring it is right —
                // a guest of this era has nothing to bind it to.
                _ => return None,
            })
        }

        fn axis(a: gilrs::Axis) -> Option<Control> {
            use gilrs::Axis as A;
            Some(match a {
                A::LeftStickX => Control::LeftStickX,
                A::LeftStickY => Control::LeftStickY,
                A::RightStickX => Control::RightStickX,
                A::RightStickY => Control::RightStickY,
                _ => return None,
            })
        }
    }

    impl Source for Gilrs {
        fn poll(&mut self, _frame: u64, out: &mut Vec<Event>) {
            while let Some(ev) = self.inner.next_event() {
                match ev.event {
                    gilrs::EventType::ButtonPressed(b, _) => {
                        if let Some(c) = Self::button(b) {
                            out.push(Event { control: c, value: 1.0 });
                        }
                    }
                    gilrs::EventType::ButtonReleased(b, _) => {
                        if let Some(c) = Self::button(b) {
                            out.push(Event { control: c, value: 0.0 });
                        }
                    }
                    // A trigger on most pads is analog and arrives here
                    // rather than as a button; past the deadzone it is a
                    // press, which `Pads` works out from the value.
                    gilrs::EventType::ButtonChanged(b, v, _) => {
                        if let Some(c) = Self::button(b) {
                            out.push(Event { control: c, value: v });
                        }
                    }
                    gilrs::EventType::AxisChanged(a, v, _) => {
                        if let Some(c) = Self::axis(a) {
                            // gilrs reports a stick pushed up as +1, the
                            // joystick convention. `Control` documents the
                            // screen convention — up is negative — so the
                            // vertical axes are flipped here, once, rather
                            // than in every consumer.
                            let v = if matches!(c, Control::LeftStickY | Control::RightStickY) {
                                -v
                            } else {
                                v
                            };
                            out.push(Event { control: c, value: v });
                        }
                    }
                    gilrs::EventType::Connected => {
                        eprintln!("[pad] connected");
                    }
                    gilrs::EventType::Disconnected => {
                        // Everything this pad was holding is now released:
                        // a guest must not be left with a key stuck down
                        // because a cable came out.
                        eprintln!("[pad] disconnected; releasing every control");
                        for c in Control::ALL {
                            out.push(Event { control: c, value: 0.0 });
                        }
                    }
                    _ => {}
                }
            }
        }

        fn describe(&self) -> String {
            let pads = self.pads();
            if pads.is_empty() {
                "gilrs, no pad connected yet".to_string()
            } else {
                format!("gilrs: {}", pads.join(", "))
            }
        }
    }
}

// --- the reader ------------------------------------------------------

/// The host end of a gamepad, however it is being read.
pub struct Pads {
    source: Box<dyn Source>,
    shaping: Shaping,
    /// The shaped value each control last reported, so `poll` can emit a
    /// change rather than a level and a consumer never sees a repeat.
    values: BTreeMap<Control, f32>,
    /// Which controls a digital consumer currently calls pressed. Kept
    /// here rather than worked out per event because hysteresis needs the
    /// previous answer (`Shaping::pressed`).
    pressed: BTreeMap<Control, bool>,
    log: bool,
}

impl Pads {
    /// Builds the reader, or returns `None` when there is nothing to read
    /// and nothing was asked for.
    ///
    /// `PLAYER_PAD_SCRIPT` wins over real hardware when both are present,
    /// because a run that sets it is a test and must not be perturbed by
    /// whatever is plugged into the machine running it.
    pub fn from_env() -> Option<Pads> {
        let log = std::env::var("PLAYER_PAD_LOG").is_ok();
        let shaping = shaping_from_env()?;
        let source: Box<dyn Source> = match Script::from_env() {
            Some(Ok(s)) => Box::new(s),
            Some(Err(e)) => {
                eprintln!("[pad] PLAYER_PAD_SCRIPT: {e}");
                return None;
            }
            None => hardware_source()?,
        };
        eprintln!("[pad] {}", source.describe());
        Some(Pads {
            source,
            shaping,
            values: BTreeMap::new(),
            pressed: BTreeMap::new(),
            log,
        })
    }

    /// Everything that changed since the last call, shaped. Call once per
    /// published guest frame from the UI thread.
    ///
    /// The UI thread and not QEMU's: on macOS a HID source needs the
    /// process's run loop, which is the main thread's, and the QEMU
    /// thread has none. It costs nothing — a pad polled at the guest's
    /// frame rate is polled far faster than a guest of this era reads
    /// one.
    pub fn poll(&mut self, frame: u64) -> Vec<Event> {
        let mut raw = Vec::new();
        self.source.poll(frame, &mut raw);
        let mut out = Vec::new();
        for ev in raw {
            let shaped = if ev.control.is_axis() {
                self.shaping.shape(ev.value)
            } else {
                ev.value
            };
            // A change and not a level: an axis resting inside the
            // deadzone shapes to 0.0 every poll, and a consumer that saw
            // all of them would be handed the same event sixty times a
            // second forever.
            //
            // A control not in the map yet counts as **centred**, not as
            // unknown. Treating it as unknown is the obvious version and
            // it is wrong: a stick resting a little off centre reports
            // itself the moment the pad is plugged in, that reading
            // shapes to 0.0, and the consumer is handed a "change" to the
            // value the control already had. Harmless for a key, and not
            // harmless at all once path B makes an axis a position the
            // guest polls.
            let previous = self.values.get(&ev.control).copied().unwrap_or(0.0);
            self.values.insert(ev.control, shaped);
            if previous == shaped {
                continue;
            }
            let was = self.pressed.get(&ev.control).copied().unwrap_or(false);
            let now = self.shaping.pressed(shaped, was);
            self.pressed.insert(ev.control, now);
            if self.log {
                eprintln!(
                    "[pad] frame {frame} {} raw {:+.3} shaped {:+.3}{}",
                    ev.control.name(),
                    ev.value,
                    shaped,
                    match (was, now) {
                        (false, true) => " press",
                        (true, false) => " release",
                        _ => "",
                    }
                );
            }
            out.push(Event {
                control: ev.control,
                value: shaped,
            });
        }
        out
    }

    /// Whether a digital consumer should currently call `control`
    /// pressed. Path C's key mapping reads this rather than the value, so
    /// the hysteresis is applied in exactly one place.
    #[allow(dead_code, reason = "path C's entry point; --pad-sweep prints it")]
    pub fn is_pressed(&self, control: Control) -> bool {
        self.pressed.get(&control).copied().unwrap_or(false)
    }

    /// Every control a digital consumer currently calls pressed, in a
    /// stable order.
    fn pressed_now(&self) -> Vec<Control> {
        Control::ALL
            .into_iter()
            .filter(|&c| self.is_pressed(c))
            .collect()
    }
}

/// `player --pads`: what this host can actually read, out of the binary
/// that has to read it.
///
/// The first question anyone asks when a pad does nothing, and the one a
/// person cannot answer from the outside: whether the controller was seen
/// at all is a fact about this build's `gilrs` feature, this host's
/// permissions and this sandbox's device access, and each of those fails
/// silently. The Flatpak without `--device=input` prints its refusal
/// here and nowhere else.
pub fn report() {
    if cfg!(not(feature = "gilrs")) {
        println!("gamepads: built without the gilrs feature; PLAYER_PAD_SCRIPT only");
        return;
    }
    match hardware_source() {
        Some(s) => println!("gamepads: {}", s.describe()),
        None => println!("gamepads: none readable on this host"),
    }
}

/// `player --pad-sweep <frames>`: run `PLAYER_PAD_SCRIPT` against a
/// counted sequence of guest frames and print what came out. No window,
/// no QEMU, no guest — the `pad` check in `scripts/test.sh`.
///
/// This is the whole host end under test: the script parser, the deadzone
/// rescale, the change filter and the hysteresis, in the same code the
/// player runs against real hardware. What it cannot test is gilrs, which
/// needs a device; everything downstream of a reading it can.
pub fn sweep(frames: u64) -> i32 {
    if std::env::var("PLAYER_PAD_SCRIPT").is_err() {
        eprintln!("--pad-sweep needs PLAYER_PAD_SCRIPT");
        return 2;
    }
    // The log is the output here, not a debug aid.
    unsafe { std::env::set_var("PLAYER_PAD_LOG", "1") };
    let Some(mut pads) = Pads::from_env() else {
        return 2;
    };
    let mut total = 0usize;
    for frame in 1..=frames {
        total += pads.poll(frame).len();
    }
    let held: Vec<&str> = pads.pressed_now().iter().map(|c| c.name()).collect();
    println!("pad-sweep: {frames} frames, {total} events");
    println!("pad-sweep: held at end: {}", if held.is_empty() { "none".to_string() } else { held.join(" ") });
    0
}

/// `PLAYER_PAD_SHAPING="deadzone,threshold,release"` overrides the three
/// numbers, for finding a worn stick's deadzone without a rebuild.
/// Absent means the defaults; malformed is refused loudly rather than
/// silently ignored, because a typo here reads as a broken pad.
fn shaping_from_env() -> Option<Shaping> {
    let Ok(spec) = std::env::var("PLAYER_PAD_SHAPING") else {
        return Some(Shaping::default());
    };
    let nums: Vec<&str> = spec.split(',').map(str::trim).collect();
    let [dz, th, rl] = nums.as_slice() else {
        eprintln!("[pad] PLAYER_PAD_SHAPING wants three numbers: deadzone,threshold,release");
        return None;
    };
    let (Ok(deadzone), Ok(threshold), Ok(release)) =
        (dz.parse::<f32>(), th.parse::<f32>(), rl.parse::<f32>())
    else {
        eprintln!("[pad] PLAYER_PAD_SHAPING: {spec:?} is not three numbers");
        return None;
    };
    let shaping = Shaping {
        deadzone,
        threshold,
        release,
    };
    if let Some(problem) = shaping.problem() {
        eprintln!("[pad] PLAYER_PAD_SHAPING: {problem}");
        return None;
    }
    Some(shaping)
}

#[cfg(feature = "gilrs")]
fn hardware_source() -> Option<Box<dyn Source>> {
    match hardware::Gilrs::new() {
        Ok(g) => Some(Box::new(g)),
        // Not fatal and not silent. A host with no input permission (the
        // Flatpak without `--device=input`) fails exactly here, and the
        // player must still run the machine — a guest is worth more than
        // a controller.
        Err(e) => {
            eprintln!("[pad] no gamepad support on this host: {e}");
            None
        }
    }
}

#[cfg(not(feature = "gilrs"))]
fn hardware_source() -> Option<Box<dyn Source>> {
    None
}
