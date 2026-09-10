//! The guided creation form (doc 07): family → name → memory →
//! processor → acceleration → networking → the pointer → disk →
//! install media → a
//! bundle written from doc 06's reference defaults. The same form edits
//! an existing bundle (`open_edit`), where `submit` writes back in place
//! instead of reserving a new library directory. An advanced toggle
//! edits the raw TOML instead — still never a QEMU command line.
//!
//! **Everything except the widgets is here**, including the sentences.
//! A front end reads `ram_note()`, `accel_note()`, `network_notes()`,
//! `seamless_mouse_notes()` and prints them; it does not compose its own. The Qt port used to, and
//! the two builds ended up telling the user different things about the
//! same checkbox ("Windows won't see a card" against "the guest won't
//! see a card"), which is a small symptom of the real problem: it also
//! had no processor, floppy or boot field at all, and its networking
//! checkbox didn't follow the family the way memory and the accelerator
//! do.
//!
//! The one asymmetry a shared form has to respect: **an immediate-mode
//! front end reads a widget's new value and compares it to the old one
//! in the same frame** (`if self.family != was { … }`), while a
//! retained-mode one has a property setter and no before/after pair. So
//! a field with a *consequence* — family, memory, acceleration,
//! networking, the processor — is private, with a `choose_*` that
//! applies the consequence and a `reset_*` that puts it back on the
//! family's default. The plain fields (a name, a path, a checkbox with
//! nothing behind it) are public and either front end writes them
//! directly.

use crate::browse::Filter;
use crate::bundle::{
    self, Accel, Boot, CpuSpeed, Family, Machine, Music, Optimization, Optimizations, Pad, Sound,
    Video,
};
use crate::disc_library::DISC_FILTER;
use crate::{host_gpu, library, player};
use std::path::{Path, PathBuf};

pub const DISK_FILTER: Filter<'static> = ("Disk images", &["qcow2", "img", "raw"]);
pub const FLOPPY_FILTER: Filter<'static> = ("Floppy images", &["img", "ima", "vfd", "flp"]);
/// A General MIDI bank for the machine's MIDI port (doc 20 §4). SF2
/// only: the engine reads SoundFont 2, and an `.sf3`'s samples are
/// Vorbis-compressed.
pub const SOUNDFONT_FILTER: Filter<'static> = ("SoundFont banks", &["sf2"]);
/// Re-exported so a front end drawing this form needs one import for its
/// three file fields; the constant itself belongs to the shelf.
pub const MEDIA_FILTER: Filter<'static> = DISC_FILTER;

/// What editing an existing bundle needs to preserve: the fields this
/// form doesn't expose, so a quick edit can't silently discard them.
/// `original_toml` is the file's exact current text, used as the
/// advanced box's starting point instead of a reconstruction — no
/// information loss even for a field a future form doesn't model.
struct EditTarget {
    bundle_path: PathBuf,
    shader: Option<PathBuf>,
    original_toml: String,
    /// The adapter the bundle had when it was opened, so the form can
    /// warn that changing it is a hardware change to a guest that is
    /// already installed (`video_warning`).
    video: Video,
    /// The same, for the sound card: a guest that already has a driver
    /// for one card finds another on its next start (`sound_warning`).
    sound: Sound,
    /// The gamepad setting the bundle had when it was opened, for the
    /// same reason: gaining or losing the USB controller is a hardware
    /// change (`pad_warning`).
    pad: Pad,
}

/// The acceleration hint under the picker, and whether it is a warning
/// (a machine that will refuse to start) rather than a note.
pub struct AccelNote {
    pub text: String,
    pub warning: bool,
}

pub struct Form {
    /// Whether the window is up. A front end owns the window; this is
    /// the model's own answer to "should it be".
    pub open: bool,
    pub name: String,
    /// A floppy image in A:, or empty for an empty drive.
    pub floppy: String,
    pub boot: Boot,
    /// The display adapter, on a family that has a choice of one
    /// (`video_applies`). Private, unlike `boot`: it is one of the four
    /// fields whose *list* changes with the family — Windows chooses
    /// between our adapter and the Cirrus, `Other` and DOS between the
    /// two standard ones — so a value carried across a family switch can
    /// be one the new family does not offer, and `choose_family` has to
    /// put it back.
    video: Video,
    /// Whether the adapter in the field is one somebody picked, the way
    /// `ram_chosen` works for the memory. Until it is, switching family
    /// moves it to the new family's default; once it is, it survives the
    /// switch — unless the new family does not offer it at all, which no
    /// flag can rescue. It was missing until 2026-09-09, so a new
    /// machine switched from 98 to XP kept the Cirrus, which is XP's
    /// *non*-default, and the same for the three fields below.
    video_chosen: bool,
    /// The sound card, and what is on the MIDI port (doc 20 §6).
    /// Private for the same reason `video` is: each family offers a
    /// different list, so a value carried across a family switch can be
    /// one the new family does not have.
    sound: Sound,
    sound_chosen: bool,
    music: Music,
    music_chosen: bool,
    /// A SoundFont bank of the user's own, or empty for the one we
    /// ship. A plain file field, like the floppy.
    pub soundfont: String,
    /// The directory holding the user's own Roland ROMs. There is no
    /// default: `Music::Mt32` without it is a machine that refuses to
    /// start, so `submit` says so instead (doc 20 §4).
    pub mt32_roms: String,
    /// What a host gamepad does for this machine (M13). Private for the
    /// same reason `video` is, and since path A for a concrete one: `Usb`
    /// is offered on the Windows families and on `Other` but never on
    /// DOS, which has no USB stack, so a value carried across a family
    /// switch can be one the new family does not offer and
    /// `choose_family` has to put it back.
    pad: Pad,
    pad_chosen: bool,
    pub existing_disk: bool,
    pub disk_path: String,
    pub disk_size_gb: u32,
    pub install_media: String,
    /// A shader profile id (`shader_library`), or `None` for the app
    /// default. Independent of `EditTarget::shader` (see `bundle::Machine`).
    pub shader_profile: Option<String>,
    pub advanced: bool,
    pub advanced_toml: String,
    /// What the last `submit` failed with, for the form to show.
    pub error: Option<String>,
    /// The bundle the last successful `submit` wrote.
    saved_path: Option<PathBuf>,

    family: Family,
    ram_mb: u32,
    /// Whether the memory field holds a value someone chose. Until it
    /// does, switching family moves it to that family's own default (doc
    /// 06), which is what picking "XP" after "Win98" means; once a
    /// number has been set, a later family switch must not throw it away.
    ram_chosen: bool,
    accel: Accel,
    /// Same as `ram_chosen`, for the accelerator: until someone picks
    /// one, the family's own default follows the family (Win98 is
    /// emulated, XP is automatic — `bundle::default_accel`).
    accel_chosen: bool,
    /// Whether the machine gets a network adapter at all. Follows the
    /// family until someone touches it, like memory and the accelerator
    /// — `bundle::default_network`, which since 2026-09-07 is off for
    /// every family. The follow is still what keeps the form and
    /// `Machine::reference` from disagreeing about a new machine, which
    /// they did, briefly, on 2026-09-06.
    network: bool,
    network_chosen: bool,
    /// Whether the host pointer walks into this machine (the USB tablet)
    /// or the window grabs it (the PS/2 mouse alone). Follows the family
    /// like the fields above: DOS cannot read a tablet at all.
    seamless_mouse: bool,
    seamless_mouse_chosen: bool,
    /// The CPU the guest should feel like, with the same rule — it is
    /// the field that makes a DOS machine a DOS machine, so switching
    /// family to DOS must bring it along.
    cpu_speed: CpuSpeed,
    cpu_speed_chosen: bool,
    /// Which of our own emulator fast paths this machine runs with.
    /// Private like the fields above, though nothing here follows the
    /// family: a checkbox that is *off* is a diagnosis someone is in the
    /// middle of, and `choose_optimization` is the only way to set one,
    /// which is what keeps the "only the difference is stored" rule
    /// (`Optimizations::set`) out of two front ends.
    optimizations: Optimizations,

    /// Whether this host can give a guest KVM. Read once when the form
    /// opens rather than per frame: it opens `/dev/kvm` to find out (a
    /// bare `exists()` misses the "not in the `kvm` group" case), and
    /// the answer cannot change while a form is on screen.
    have_kvm: bool,
    /// The same, for the guest's 3D (`host_gpu`, ADR-013), and for the
    /// same reason twice over: a probe is a whole `VkInstance`, and the
    /// answer cannot change while a form is on screen. `cached` so that
    /// every window opened in one session pays for it once.
    host_gpu: host_gpu::HostGpu,
    editing: Option<EditTarget>,
}

impl Default for Form {
    fn default() -> Self {
        Form {
            open: false,
            name: String::new(),
            floppy: String::new(),
            boot: Boot::default(),
            video: bundle::default_video(Family::Win98).unwrap_or(Video::Std),
            video_chosen: false,
            sound: bundle::default_sound(Family::Win98),
            sound_chosen: false,
            music: bundle::default_music(Family::Win98),
            music_chosen: false,
            soundfont: String::new(),
            mt32_roms: String::new(),
            pad: bundle::default_pad(Family::Win98),
            pad_chosen: false,
            existing_disk: false,
            disk_path: String::new(),
            disk_size_gb: 2,
            install_media: String::new(),
            shader_profile: None,
            advanced: false,
            advanced_toml: String::new(),
            error: None,
            saved_path: None,
            family: Family::Win98,
            ram_mb: bundle::default_ram_mb(Family::Win98),
            ram_chosen: false,
            accel: bundle::default_accel(Family::Win98),
            accel_chosen: false,
            network: bundle::default_network(Family::Win98),
            network_chosen: false,
            seamless_mouse: bundle::default_seamless_mouse(Family::Win98),
            seamless_mouse_chosen: false,
            cpu_speed: bundle::default_cpu_speed(Family::Win98),
            cpu_speed_chosen: false,
            optimizations: Optimizations::default(),
            have_kvm: player::hw_accel_available(),
            host_gpu: host_gpu::cached().gpu,
            editing: None,
        }
    }
}

// --- opening -------------------------------------------------------

impl Form {
    /// Reset to a fresh "New machine" form and open it.
    pub fn open_fresh(&mut self) {
        *self = Form { open: true, ..Default::default() };
    }

    /// The same, starting on a given family — everything that follows
    /// from it (memory, the accelerator, the processor, the NIC) comes
    /// with it.
    pub fn open_new(&mut self, family: Family) {
        self.open_fresh();
        self.choose_family(family);
    }

    /// Open the form pre-filled from an existing bundle, to edit it in
    /// place instead of creating a new one.
    pub fn open_edit(&mut self, machine: &Machine, bundle_path: PathBuf) {
        let original_toml = std::fs::read_to_string(&bundle_path).unwrap_or_default();
        *self = Form {
            open: true,
            name: machine.name.clone(),
            family: machine.family,
            ram_mb: machine.ram_mb,
            // An existing machine's memory is a chosen value, whatever
            // it came from: changing family must not rewrite it. Same
            // for the other three.
            ram_chosen: true,
            accel: machine.effective_accel(),
            accel_chosen: true,
            network: machine.network,
            network_chosen: true,
            seamless_mouse: machine.seamless_mouse,
            seamless_mouse_chosen: true,
            cpu_speed: machine.effective_cpu_speed(),
            cpu_speed_chosen: true,
            optimizations: machine.optimizations.clone(),
            floppy: machine.floppy.as_ref().map(|f| f.display().to_string()).unwrap_or_default(),
            boot: machine.effective_boot(),
            // An existing machine's adapter, card, MIDI port and pad are
            // chosen values for the same reason its memory is: whatever
            // they came from, switching family must not rewrite them.
            video: machine.effective_video().unwrap_or(Video::Std),
            video_chosen: true,
            sound: machine.effective_sound(),
            sound_chosen: true,
            music: machine.effective_music(),
            music_chosen: true,
            soundfont: machine.soundfont.as_ref().map(|f| f.display().to_string()).unwrap_or_default(),
            mt32_roms: machine.mt32_roms.as_ref().map(|d| d.display().to_string()).unwrap_or_default(),
            pad: machine.effective_pad(),
            pad_chosen: true,
            existing_disk: true,
            disk_path: machine.disk.display().to_string(),
            install_media: machine.boot_disc().map(|d| d.display().to_string()).unwrap_or_default(),
            shader_profile: machine.shader_profile.clone(),
            editing: Some(EditTarget {
                bundle_path,
                shader: machine.shader.clone(),
                original_toml,
                video: machine.effective_video().unwrap_or(Video::Std),
                sound: machine.effective_sound(),
                pad: machine.effective_pad(),
            }),
            ..Default::default()
        };
    }

    /// The same, from a bundle path — what a front end that addresses
    /// windows by path (the Qt one does; every window there re-reads the
    /// bundle rather than being handed a copy) needs. The load error, if
    /// any, lands in `error` and the form does not open.
    pub fn open_edit_path(&mut self, bundle_path: PathBuf) {
        match Machine::load(&bundle_path) {
            Ok(machine) => self.open_edit(&machine, bundle_path),
            Err(e) => {
                self.open = false;
                self.error = Some(format!("{}: {e}", bundle_path.display()));
            }
        }
    }

    /// Headless construction with the same fields the widgets would have
    /// set, so `submit`'s real disk-creation and save logic can be
    /// exercised without a GUI click (`cli`'s `--wizard-new`).
    pub fn with_new_disk(family: Family, name: String, disk_size_gb: u32) -> Form {
        let mut form = Form { name, disk_size_gb, ..Default::default() };
        form.choose_family(family);
        form
    }

    pub fn is_editing(&self) -> bool {
        self.editing.is_some()
    }

    /// "Edit machine" or "New machine" — the window's own title.
    pub fn title(&self) -> &'static str {
        if self.is_editing() {
            "Edit machine"
        } else {
            "New machine"
        }
    }
}

// --- the fields with a consequence ----------------------------------

impl Form {
    pub fn family(&self) -> Family {
        self.family
    }

    /// Pick the family, moving whatever nobody has chosen to that
    /// family's own default with it.
    pub fn choose_family(&mut self, family: Family) {
        self.family = family;
        if !self.ram_chosen {
            self.ram_mb = bundle::default_ram_mb(family);
        }
        if !self.accel_chosen {
            self.accel = bundle::default_accel(family);
        }
        if !self.cpu_speed_chosen {
            self.cpu_speed = bundle::default_cpu_speed(family);
        }
        if !self.network_chosen {
            self.network = bundle::default_network(family);
        }
        if !self.seamless_mouse_chosen {
            self.seamless_mouse = bundle::default_seamless_mouse(family);
        }
        // The adapter, the card, the MIDI port and the gamepad take the
        // new family's default on **either** of two counts, where the
        // fields above have only the first. Nobody has picked one, so it
        // is following the family like everything else — a new machine
        // moved from 98 to XP has to arrive on XP's adapter, not sit on
        // the Cirrus that was 98's. Or somebody did pick one and the new
        // family does not offer it at all, which no flag can rescue: our
        // own adapter is not on offer for BeOS, a Gravis is not on offer
        // to XP, an ES1370 not to DOS, and a `Usb` pad needs a USB stack
        // DOS has not got.
        if !self.sound_chosen || !bundle::sound_choices(family).contains(&self.sound) {
            self.sound = bundle::default_sound(family);
        }
        if !self.music_chosen || !bundle::music_choices(family).contains(&self.music) {
            self.music = bundle::default_music(family);
        }
        if !self.video_chosen || !bundle::video_choices(family).contains(&self.video) {
            if let Some(default) = bundle::default_video(family) {
                self.video = default;
            }
        }
        if !self.pad_chosen || !bundle::pad_choices(family).contains(&self.pad) {
            self.pad = bundle::default_pad(family);
        }
        // The new family's ceiling may be below the memory already in
        // the field (Win98 stops at 512 MB), so the clamp is part of the
        // switch rather than something the drawing code remembers to do.
        self.ram_mb = self.ram_mb.clamp(*self.ram_range().start(), *self.ram_range().end());
    }

    /// What picking this family means, under the picker. Only `Other`
    /// has anything to say: the other three *are* the reference machines
    /// doc 06 describes and everything else on the form explains itself,
    /// while `Other` is defined by the two things it does not get — our
    /// display driver and the 3D pass-through, both of which are
    /// Windows-only — and by hardware chosen for guests we cannot test
    /// here. Someone finding out afterwards would find out by installing
    /// an OS onto it.
    pub fn family_note(&self) -> Option<&'static str> {
        (self.family == Family::Other).then_some(
            "For an era OS that isn't Windows or DOS: BeOS, a period Linux, OS/2. \
             Standard hardware only — a VESA-capable VGA, an RTL8139 and an ES1370 \
             sound card, all of which these systems have drivers for in the box.\n\
             No 3D: our display driver and the Direct3D and Glide pass-through are \
             Windows components, so this family is 2D, the CRT shaders and the CD-ROM drive.",
        )
    }

    pub fn ram_mb(&self) -> u32 {
        self.ram_mb
    }

    pub fn ram_range(&self) -> std::ops::RangeInclusive<u32> {
        bundle::ram_mb_range(self.family)
    }

    pub fn ram_is_default(&self) -> bool {
        self.ram_mb == bundle::default_ram_mb(self.family)
    }

    /// Set the memory, clamped to the family's range — a value outside
    /// it is corrected here rather than refused at save time, so the
    /// form cannot produce a Win98 machine with more memory than Win98
    /// can boot with.
    pub fn choose_ram_mb(&mut self, ram_mb: u32) {
        let range = self.ram_range();
        self.ram_mb = ram_mb.clamp(*range.start(), *range.end());
        self.ram_chosen = true;
    }

    pub fn reset_ram(&mut self) {
        self.ram_mb = bundle::default_ram_mb(self.family);
        self.ram_chosen = false;
    }

    pub fn ram_note(&self) -> Option<&'static str> {
        match self.family {
            Family::Win98 if self.ram_mb >= *self.ram_range().end() => {
                Some("512 MB is Win98's ceiling (doc 06): more and it does not boot.")
            }
            // The range cannot enforce this one — it depends on which OS
            // is going in, and the whole point of the family is that we
            // don't know. A period Linux is happy with 3 GB.
            Family::Other if self.ram_mb > 1024 => {
                Some("Above 1 GB, BeOS R5 does not boot; most other systems of the era are fine.")
            }
            _ => None,
        }
    }

    pub fn cpu_speed(&self) -> CpuSpeed {
        self.cpu_speed
    }

    pub fn cpu_speed_is_default(&self) -> bool {
        self.cpu_speed == bundle::default_cpu_speed(self.family)
    }

    pub fn choose_cpu_speed(&mut self, cpu_speed: CpuSpeed) {
        self.cpu_speed = cpu_speed;
        self.cpu_speed_chosen = true;
    }

    pub fn reset_cpu_speed(&mut self) {
        self.cpu_speed = bundle::default_cpu_speed(self.family);
        self.cpu_speed_chosen = false;
    }

    /// What follows from the chosen processor, said before the machine
    /// is created rather than after it behaves oddly.
    pub fn cpu_speed_notes(&self) -> &'static [&'static str] {
        if self.cpu_speed == CpuSpeed::Unthrottled {
            &["Full speed. Right for Windows; most DOS software of the 486 era needs a slower one."]
        } else {
            &[
                "DOS-era software times itself against the CPU it finds, so this is what makes a game playable.",
                "A chosen processor means the machine is emulated: QEMU cannot slow a CPU down under KVM.",
            ]
        }
    }

    pub fn accel(&self) -> Accel {
        self.accel
    }

    pub fn accel_is_default(&self) -> bool {
        self.accel == bundle::default_accel(self.family)
    }

    pub fn choose_accel(&mut self, accel: Accel) {
        self.accel = accel;
        self.accel_chosen = true;
    }

    pub fn reset_accel(&mut self) {
        self.accel = bundle::default_accel(self.family);
        self.accel_chosen = false;
    }

    /// Whether this host has hardware acceleration — the picker alone
    /// would leave "Automatic" meaning something invisible.
    pub fn have_kvm(&self) -> bool {
        self.have_kvm
    }

    pub fn accel_note(&self) -> AccelNote {
        // KVM on Linux, WHPX on Windows: the note names what this host
        // has, because "No KVM on this host" on a Windows machine is
        // both wrong and unactionable.
        let hw = player::hw_accel_label().unwrap_or("Hardware acceleration");
        let mut text = match (self.accel, self.have_kvm) {
            (Accel::Auto, true) => format!("{hw} is available on this host and will be used."),
            (Accel::Auto, false) => format!("No {hw} on this host: this machine will be emulated."),
            (Accel::Kvm, true) => format!("{hw} is available on this host."),
            (Accel::Kvm, false) => format!("No {hw} on this host: this machine will refuse to start."),
            (Accel::Tcg, _) => "Emulated: the era-CPU behaviour everything here is tuned for.".to_string(),
        };
        if self.family == Family::Win98 && self.accel != Accel::Tcg && self.have_kvm {
            text.push('\n');
            text.push_str(&format!("Win98 runs at host speed under {hw}, which its own fast-CPU bugs dislike."));
        }
        AccelNote { text, warning: matches!((self.accel, self.have_kvm), (Accel::Kvm, false)) }
    }

    /// What this host will give the guest's 3D, under the acceleration
    /// row (ADR-013). Not a picker, because there is nothing to pick: the
    /// host settles it, and the only failure worth preventing is finding
    /// out after the machine exists. The two families with no Direct3D to
    /// place get no line: DOS, and `Other` — whose guests cannot reach
    /// the pass-through at all, since the guest half of it is a set of
    /// Windows DLLs (`family_note` says so once, where the choice is
    /// made).
    ///
    /// `warning` is true only for a software Vulkan driver — the
    /// executor does run there, and slowly, which is the one case where
    /// what the user sees will disappoint them. A host with no Vulkan at
    /// all is a plain note: it runs every machine, through the OpenGL
    /// pass-through with WineD3D in the guest, and nothing is wrong.
    pub fn graphics_note(&self) -> Option<AccelNote> {
        if matches!(self.family, Family::Dos | Family::Other) {
            return None;
        }
        let mut text = format!("3D: {}", self.host_gpu.headline());
        if let Some(advice) = self.host_gpu.advice() {
            text.push('\n');
            text.push_str(advice);
        }
        Some(AccelNote { text, warning: self.host_gpu.is_slow() })
    }

    pub fn network(&self) -> bool {
        self.network
    }

    pub fn choose_network(&mut self, network: bool) {
        self.network = network;
        self.network_chosen = true;
    }

    /// One checkbox, because there is one question: does this machine
    /// have a network card. What it gets when it does is QEMU's
    /// user-mode NAT (doc 06's per-family NIC), which needs no host
    /// privileges and gives nothing on the network a way in — worth
    /// saying, since "networking" otherwise sounds like the guest is
    /// being put on the LAN. And worth saying once, next to the switch,
    /// that these guests stopped getting security fixes twenty years ago.
    pub fn network_notes(&self) -> &'static [&'static str] {
        if self.network {
            &[
                "Outbound only, through the host (user-mode NAT): nothing on the network can reach the guest.",
                "These are unpatched systems — don't browse the web on one.",
            ]
        } else {
            &["No network adapter at all: the guest won't see a card or ask for its driver."]
        }
    }

    pub fn seamless_mouse(&self) -> bool {
        self.seamless_mouse
    }

    pub fn choose_seamless_mouse(&mut self, seamless_mouse: bool) {
        self.seamless_mouse = seamless_mouse;
        self.seamless_mouse_chosen = true;
    }

    /// One checkbox again, because there is one question: does the host
    /// pointer walk into this machine, or does the window take it. Both
    /// answers are right for something — a desktop wants the first, a
    /// game that turns the view with the mouse needs the second — so
    /// what the sentences have to carry is the hotkey (a grabbed pointer
    /// with no way out is the worst thing this form can produce) and the
    /// symptom that sends someone back here: mouselook against an
    /// absolute device does not turn, it sticks.
    ///
    /// DOS is worth catching before the machine exists rather than
    /// after: its mouse drivers read the PS/2 controller, so a tablet
    /// leaves such a guest with no pointer at all. `Other` ships with the
    /// tablet off for the softer version of that (`default_seamless_mouse`)
    /// and says what to look for if someone turns it on.
    pub fn seamless_mouse_notes(&self) -> &'static [&'static str] {
        match (self.seamless_mouse, self.family) {
            (true, Family::Dos) => &[
                "The host pointer moves straight into the guest, with no grab and no hotkey.",
                "DOS mouse drivers read the PS/2 mouse: on this family the tablet leaves the guest with no pointer at all.",
            ],
            (true, Family::Other) => &[
                "The host pointer moves straight into the guest, with no grab and no hotkey: the guest gets a USB tablet, which reports where the pointer is rather than how far it moved.",
                "Whether it works is the guest's affair here — an absolute pointer needs its USB stack and its windowing system to agree, which BeOS and an era XFree86 do not do unconfigured. Turn it off if the cursor doesn't move.",
            ],
            (true, _) => &[
                "The host pointer moves straight into the guest, with no grab and no hotkey: the guest gets a USB tablet, which reports where the pointer is rather than how far it moved.",
            ],
            (false, _) => &[
                "The PS/2 mouse alone: click the window to take the pointer, Ctrl+Alt+G to give it back.",
                "That is the relative movement mouselook needs — a game whose view sticks instead of turning wants this off.",
            ],
        }
    }

    pub fn optimizations(&self) -> &Optimizations {
        &self.optimizations
    }

    pub fn optimization_enabled(&self, opt: Optimization) -> bool {
        self.optimizations.enabled(opt)
    }

    pub fn choose_optimization(&mut self, opt: Optimization, on: bool) {
        self.optimizations.set(opt, on);
    }

    /// Every fast path back on its shipped setting — the way out of a
    /// diagnosis session, and the only button in that section that
    /// someone will look for.
    pub fn reset_optimizations(&mut self) {
        self.optimizations.reset();
    }

    pub fn optimizations_are_default(&self) -> bool {
        self.optimizations.all_default()
    }

    /// What the collapsed section says about itself, so a machine with
    /// something turned off says so without being opened.
    pub fn optimizations_summary(&self) -> String {
        self.optimizations.summary()
    }

    /// The one thing worth saying above the switches: on a machine that
    /// is about to run on KVM they are all inert, because the guest's
    /// instructions are then executed by the host CPU and there is no
    /// emulator in the path to have a fast path.
    pub fn optimizations_note(&self) -> &'static str {
        if self.will_use_kvm() {
            "This machine runs on KVM, where none of these apply: they are fast paths in the emulator. \
             Choose Emulation above (or a processor, which forces it) to use them."
        } else {
            "Our own additions to QEMU, each measured (patches/qemu/README.md). Turn one off to find out \
             whether it is what makes a guest compute the wrong number or stop drawing."
        }
    }

    /// Whether this machine, as the form currently stands, will actually
    /// run on KVM: what `effective_accel` decides, plus what this host
    /// has — `Automatic` on a box without `/dev/kvm` is emulation.
    fn will_use_kvm(&self) -> bool {
        self.cpu_speed.icount_shift().is_none()
            && match self.accel {
                Accel::Kvm => true,
                Accel::Auto => self.have_kvm,
                Accel::Tcg => false,
            }
    }

    pub fn video(&self) -> Video {
        self.video
    }

    /// Which adapters this machine's family offers, first one its
    /// default — what a picker fills itself from, so no front end has to
    /// know that Windows is offered a different pair than `Other`.
    pub fn video_choices(&self) -> &'static [Video] {
        bundle::video_choices(self.family)
    }

    /// Whether there is an adapter to choose at all, so a front end
    /// shows or hides the row without knowing which family that is.
    /// Every family offers a pair today — DOS since 2026-09-09 — so this
    /// is true throughout; it stays because the answer is
    /// `video_choices`'s to give and a family that gains a fixed adapter
    /// should not need a front end changed.
    pub fn video_applies(&self) -> bool {
        !self.video_choices().is_empty()
    }

    pub fn video_is_default(&self) -> bool {
        Some(self.video) == bundle::default_video(self.family)
    }

    /// An adapter this family does not offer is refused rather than
    /// stored: the list is per family and a front end may be a frame
    /// behind on it.
    pub fn choose_video(&mut self, video: Video) {
        if self.video_choices().contains(&video) {
            self.video = video;
            self.video_chosen = true;
        }
    }

    /// Back to this family's default — and back to *following* the
    /// family, so a later switch moves it again. "Default" means the
    /// field was never really touched.
    pub fn reset_video(&mut self) {
        if let Some(default) = bundle::default_video(self.family) {
            self.video = default;
            self.video_chosen = false;
        }
    }

    /// What the chosen adapter means *for this family*. The same Cirrus
    /// is Windows' in-box driver on one machine and a period XFree86
    /// driver on another, and what it costs is different too: on Windows
    /// it is the whole display path (docs 15, 19) that goes with it.
    pub fn video_notes(&self) -> &'static [&'static str] {
        match (self.video, self.family) {
            (Video::D3dpt, _) => &[
                "Our own adapter and display driver: the mode table, the desktop straight from video memory, the page flips that pace a game, and Direct3D through the driver itself.",
                "It needs the driver installed from the guest-tools ISO. Until it is, the guest comes up on the plain VGA the same device also is.",
            ],
            (Video::Cirrus, Family::Win98) => &[
                "The Cirrus GD5446, which Windows 98 has a driver for in the box, and where a new 98 machine starts: 2D only, and none of our display path — no mode table, no paced flips, no Direct3D through the driver.",
                "Our own adapter is the step up from it, once its driver is installed from the guest-tools ISO.",
            ],
            (Video::Cirrus, Family::Xp) => &[
                "The Cirrus GD5446, which Windows has a driver for in the box: 2D only, and none of our display path — no mode table, no paced flips, no Direct3D through the driver.",
                "The right answer for a machine whose driver isn't installed yet, and the A/B for a title that misbehaves on ours.",
            ],
            (Video::Cirrus, Family::Dos) => &[
                "The Cirrus GD5446 and its period VGA/VESA BIOS, which is what a DOS machine here has always had and where this family starts.",
                "Nothing is installed either way — a DOS title programs the adapter itself — so the standard VGA is one restart away and back if a game's modes come out wrong on this one.",
            ],
            (Video::Std, Family::Dos) => &[
                "The Bochs adapter: VBE 2.0 and a linear frame buffer, the later and fuller of the two VESA BIOSes a DOS title can find here.",
                "Worth trying when a game's high-resolution modes are wrong or missing on the Cirrus. It is not the safer answer, just the other one — some titles know the Cirrus and not this.",
            ],
            (Video::Cirrus, _) => &[
                "A chip that really existed, so a guest of the era is likely to have a native driver for it: BeOS R5 and XFree86 both ship one.",
                "In exchange it is the weaker VESA adapter of the two. Try it when the standard VGA leaves the guest in plain VGA.",
            ],
            (Video::Std, _) => &[
                "The Bochs adapter: VBE 2.0 and a linear frame buffer, which is what a period VESA driver wants and what a modern Linux binds bochs-drm to.",
                "The safe answer — a guest with no native driver still gets its VESA modes.",
            ],
        }
    }

    // --- the sound card and the MIDI port (doc 20 §6) ---------------

    pub fn sound(&self) -> Sound {
        self.sound
    }

    /// The cards this family offers. Never empty: "no sound card" is an
    /// entry, so every family has a picker.
    pub fn sound_choices(&self) -> &'static [Sound] {
        bundle::sound_choices(self.family)
    }

    pub fn sound_is_default(&self) -> bool {
        self.sound == bundle::default_sound(self.family)
    }

    pub fn choose_sound(&mut self, sound: Sound) {
        if self.sound_choices().contains(&sound) {
            self.sound = sound;
            self.sound_chosen = true;
        }
    }

    pub fn reset_sound(&mut self) {
        self.sound = bundle::default_sound(self.family);
        self.sound_chosen = false;
    }

    pub fn music(&self) -> Music {
        self.music
    }

    pub fn music_choices(&self) -> &'static [Music] {
        bundle::music_choices(self.family)
    }

    pub fn music_is_default(&self) -> bool {
        self.music == bundle::default_music(self.family)
    }

    pub fn choose_music(&mut self, music: Music) {
        if self.music_choices().contains(&music) {
            self.music = music;
            self.music_chosen = true;
        }
    }

    pub fn reset_music(&mut self) {
        self.music = bundle::default_music(self.family);
        self.music_chosen = false;
    }

    /// Whether the SoundFont field is worth showing at all.
    pub fn soundfont_applies(&self) -> bool {
        self.music == Music::Gm
    }

    /// Whether the ROM directory field is, and it is not optional there.
    pub fn mt32_roms_applies(&self) -> bool {
        self.music == Music::Mt32
    }

    /// What the card does for the guest's *music*, which is the half of
    /// this screen that is not obvious: whether the machine has FM at
    /// all, and what the guest has to do before it hears anything.
    pub fn sound_notes(&self) -> &'static [&'static str] {
        match (self.sound, self.family) {
            (Sound::Sb16, Family::Dos) => &[
                "The card DOS titles know how to find, and its OPL3: AdLib music works with nothing installed.",
                "Its line for AUTOEXEC.BAT is BLASTER=A220 I5 D1 H5 P330 T6 — the P330 is what points a game at the MIDI port.",
            ],
            (Sound::Sb16, _) => &[
                "Windows has this driver in the box, and a DOS box inside the guest finds the card it expects.",
                "It carries the OPL3, so a game that only knows AdLib music has something to play on.",
            ],
            (Sound::Ac97, Family::Win98) => &[
                "Better sound than the SB16, but 98 has no driver for it in the box — install ours from the guest tools first.",
                "No FM chip: a DOS game inside this machine will find no AdLib music. Its MIDI port still works.",
            ],
            (Sound::Ac97, _) => &[
                "XP's own driver, and the card this family has always had.",
                "No FM chip — nothing of the era needs one here.",
            ],
            (Sound::Es1370, _) => &[
                "The PCI card of the period BeOS R5 and a period Linux both drive with a driver they already have.",
            ],
            (Sound::Gus, _) => &[
                "A wavetable card: its music is its own, and the games written for one sound better on it than on anything else of the era.",
                "The guest needs Gravis's own drivers and its ULTRASND line before it makes any sound at all.",
                "No FM chip, and no Sound Blaster compatibility except through Gravis's own emulation.",
            ],
            (Sound::Adlib, _) => &[
                "The 1990 machine: FM music and no digital audio at all, so a game's speech and sound effects will be silent.",
            ],
            (Sound::None, _) => &[
                "No card at all. The machine can still have a MIDI port, which is how music was done before cards could play samples.",
            ],
        }
    }

    /// What is behind the MIDI port, and what it needs.
    pub fn music_notes(&self) -> &'static [&'static str] {
        match (self.music, self.family) {
            (Music::Gm, Family::Win98) => &[
                "An MPU-401 at 0x330 with a General MIDI synthesizer behind it — far better than the FM chip, which is what 98's own MIDI output uses otherwise.",
                "Windows finds it only after \"MPU-401 Compatible\" is added from Add New Hardware, and picked in Multimedia.",
                "Leave the bank empty for the one we ship; a bank of your own changes how everything sounds more than any other setting here.",
            ],
            (Music::Gm, _) => &[
                "An MPU-401 at 0x330 with a General MIDI synthesizer behind it: what a game means by \"General MIDI\" or \"MPU-401\" on its setup screen.",
                "Leave the bank empty for the one we ship; a bank of your own changes how everything sounds more than any other setting here.",
            ],
            (Music::Mt32, _) => &[
                "What a 1990 title means by \"Roland\": an MT-32 family module, which its music was written for and which sounds nothing like General MIDI.",
                "It needs your own Roland CM-32L ROM images — nothing of Roland's is shipped with this program — and the machine will not start without them.",
            ],
            (Music::None, _) => &[
                "No MIDI port. A game offering General MIDI or Roland will find nothing and fall back to its FM or digital music.",
            ],
        }
    }

    /// The same warning the adapter carries, for the same reason: a
    /// guest that is already installed re-detects a card that changed.
    pub fn sound_warning(&self) -> Option<&'static str> {
        let changed = match &self.editing {
            Some(edit) => edit.sound != self.sound,
            None => false,
        };
        changed.then_some(
            "This machine already exists: changing its sound card makes the guest find new hardware on its next start, \
             and a game inside it will have to be told about the new card as well.",
        )
    }

    /// The one thing worth saying above the picker rather than under one
    /// of its entries: changing this on a machine that already has an OS
    /// installed is a hardware change, and the guest will say so.
    ///
    /// Except on DOS, where it is not: nothing is installed for an
    /// adapter there, the machine simply boots. What can still be stale
    /// is a *game's* own setup — a title that has already been through
    /// its SETUP wrote down a video mode, and the two adapters do not
    /// offer the same list — so that is what DOS is told instead.
    pub fn video_warning(&self) -> Option<&'static str> {
        if !self.is_editing() || self.video_is_default_for_machine() {
            return None;
        }
        Some(match self.family {
            Family::Dos => {
                "This machine already exists: it will boot on the new adapter with nothing to install, \
                 but a game that has already run its own setup may have recorded a video mode this one \
                 does not offer, and want that setup run again."
            }
            _ => {
                "This machine already exists: changing its adapter makes the guest find new hardware on its next start, \
                 and it will want a driver for it before the desktop comes back."
            }
        })
    }

    /// Whether the adapter is still the one the bundle was opened with.
    fn video_is_default_for_machine(&self) -> bool {
        match &self.editing {
            Some(edit) => edit.video == self.video,
            None => true,
        }
    }

    pub fn pad(&self) -> Pad {
        self.pad
    }

    /// What this family offers a gamepad, first one its default — what a
    /// picker fills itself from.
    pub fn pad_choices(&self) -> &'static [Pad] {
        bundle::pad_choices(self.family)
    }

    /// Whether there is anything to choose. True on every family — even
    /// DOS has `None` against `Keys` — but written against the model so a
    /// front end's row does not depend on that staying true.
    pub fn pad_applies(&self) -> bool {
        self.pad_choices().len() > 1
    }

    pub fn pad_is_default(&self) -> bool {
        self.pad == bundle::default_pad(self.family)
    }

    /// A setting this family does not offer is refused rather than
    /// stored, the same way `choose_video` refuses an adapter.
    pub fn choose_pad(&mut self, pad: Pad) {
        if self.pad_choices().contains(&pad) {
            self.pad = pad;
            self.pad_chosen = true;
        }
    }

    pub fn reset_pad(&mut self) {
        self.pad = bundle::default_pad(self.family);
        self.pad_chosen = false;
    }

    /// What the chosen setting means. `Keys` needs its limitation said
    /// plainly and in the picker, not discovered: someone who turns it on
    /// for a Direct3D game will otherwise conclude the pad is broken,
    /// when what is actually true is that the game asked DirectInput and
    /// there is no controller for it to find yet.
    pub fn pad_notes(&self) -> &'static [&'static str] {
        match self.pad {
            Pad::None => &[
                "A controller plugged into the host does nothing. The machine's keyboard and mouse are unaffected.",
            ],
            Pad::Usb => &[
                "A real USB controller on the machine: two analog sticks, an 8-way hat and twelve buttons, which DirectInput and the Game Controllers panel both see.",
                "Windows XP, 98 SE and Me need nothing installed — they bind their own HID driver to it on the first start after it is added. Windows 98 first edition may want the USB supplement.",
            ],
            Pad::Keys => &[
                "The pad presses keys: the d-pad and left stick are the arrow keys, and the four face buttons are Ctrl, Alt, Space and Enter — what a DOS or early-Windows action game reads by default.",
                "It is a mapping, not a controller: no analog steering, and a game that asks DirectInput for a joystick still finds none. The choice for DOS, and for a game that only ever read the keyboard.",
            ],
        }
    }

    /// The one thing worth saying above the picker: adding or removing a
    /// USB controller on a machine that already has an OS installed is a
    /// hardware change, and the guest will notice on its next start. The
    /// same sentence the adapter picker earns, for the same reason.
    pub fn pad_warning(&self) -> Option<&'static str> {
        let changed = match &self.editing {
            Some(edit) => (edit.pad == Pad::Usb) != (self.pad == Pad::Usb),
            None => false,
        };
        changed.then_some(
            "This machine already exists: adding or removing the USB controller makes the guest \
             find new hardware on its next start. Windows installs its own driver for it, but it \
             will say so.",
        )
    }

    /// The one thing the boot picker can say that isn't obvious: a
    /// machine told to boot from a floppy it hasn't got.
    pub fn boot_note(&self) -> Option<&'static str> {
        (self.boot == Boot::Floppy && self.floppy.trim().is_empty())
            .then_some("No floppy image: the machine will fall through to the hard disk.")
    }
}

// --- what it writes -------------------------------------------------

impl Form {
    /// The `Machine` the current field values describe, given the disk
    /// path to use (a fresh disk's path isn't known until it's created,
    /// so callers that might still need to do that pass it in). Shared
    /// by the advanced box's default and the plain submit path, so the
    /// two cannot silently disagree.
    pub fn build_machine(&self, disk: PathBuf) -> Machine {
        let mut machine = match &self.editing {
            Some(edit) => Machine {
                name: self.name.clone(),
                family: self.family,
                ram_mb: self.ram_mb,
                accel: Some(self.accel),
                network: self.network,
                seamless_mouse: self.seamless_mouse,
                disk,
                disc: None,
                discs: Vec::new(),
                shader_profile: None,
                shader: edit.shader.clone(),
                floppy: None,
                boot: None,
                cpu_speed: None,
                video: None,
                sound: None,
                music: None,
                soundfont: None,
                mt32_roms: None,
                pad: None,
                optimizations: Optimizations::default(),
            },
            None => Machine::reference(self.family, self.name.clone(), disk),
        };
        // The form owns these for a new machine too, where `reference`
        // has just filled in the family defaults. The `*_chosen` flag
        // decides, not the field's current contents: a form constructed
        // without going through the family picker (`with_new_disk`, the
        // headless verb) never had the chance to move the default along
        // with it, and silently writing Win98's 256 MB into an XP
        // machine is exactly the bug that produced.
        machine.ram_mb = if self.ram_chosen { self.ram_mb } else { bundle::default_ram_mb(self.family) };
        // Written out explicitly either way: what the form showed is
        // what the machine gets, even when it is the family's default.
        machine.accel = Some(if self.accel_chosen { self.accel } else { bundle::default_accel(self.family) });
        machine.network = if self.network_chosen { self.network } else { bundle::default_network(self.family) };
        machine.seamless_mouse = if self.seamless_mouse_chosen {
            self.seamless_mouse
        } else {
            bundle::default_seamless_mouse(self.family)
        };
        machine.cpu_speed =
            Some(if self.cpu_speed_chosen { self.cpu_speed } else { bundle::default_cpu_speed(self.family) });
        // Only what someone turned off is in here, so this is a clone
        // of "nothing" for almost every machine (`Optimizations`).
        machine.optimizations = self.optimizations.clone();
        machine.boot = Some(self.boot);
        // Written only on a family that has a choice, so switching a
        // machine to DOS cannot leave a `video` behind that the family
        // ignores and the next reader has to wonder about.
        machine.video = bundle::video_choices(self.family).contains(&self.video).then_some(self.video);
        // Written out explicitly, like the accelerator: what the form
        // showed is what the machine gets, even when it is the family's
        // default — a bundle that names its card cannot be changed
        // under the user by a later change to what that default is.
        machine.sound = Some(self.sound);
        machine.music = Some(self.music);
        // Only the user's own files, and only where they mean anything:
        // a bank left behind on a machine whose port was turned off is a
        // field the next reader has to wonder about.
        machine.soundfont = (self.music == Music::Gm)
            .then(|| Some(self.soundfont.trim()).filter(|f| !f.is_empty()).map(PathBuf::from))
            .flatten();
        machine.mt32_roms = (self.music == Music::Mt32)
            .then(|| Some(self.mt32_roms.trim()).filter(|d| !d.is_empty()).map(PathBuf::from))
            .flatten();
        // Same rule as `video`: only a setting this family offers is
        // written. Every family offers both today, so this always
        // writes; the guard is here for when path A makes `Usb` a
        // Windows-only entry and a machine switched to DOS must not
        // keep it.
        machine.pad = bundle::pad_choices(self.family).contains(&self.pad).then_some(self.pad);
        machine.floppy = Some(self.floppy.trim()).filter(|f| !f.is_empty()).map(PathBuf::from);
        machine.shader_profile = self.shader_profile.clone();
        // The single slot this form has is the machine's *boot* disc;
        // everything else lives on the shared shelf (`disc_library.rs`).
        machine.disc = Some(self.install_media.trim()).filter(|m| !m.is_empty()).map(PathBuf::from);
        machine
    }

    /// What the advanced box opens on: the file's exact current text
    /// when editing, the TOML this form describes when creating.
    pub fn preview_toml(&self) -> String {
        if let Some(edit) = &self.editing {
            return edit.original_toml.clone();
        }
        let disk: PathBuf =
            if self.existing_disk { self.disk_path.clone().into() } else { "disk.qcow2".into() };
        toml::to_string_pretty(&self.build_machine(disk)).unwrap_or_default()
    }

    /// Fill the advanced box if it is still empty. Called when the
    /// toggle goes on: an immediate-mode front end does it while
    /// drawing, a retained-mode one from the checkbox's handler.
    pub fn fill_advanced(&mut self) {
        if self.advanced_toml.is_empty() {
            self.advanced_toml = self.preview_toml();
        }
    }

    /// Create or save the machine, closing the form on success. `None`
    /// leaves it open with `error` saying why.
    pub fn submit(&mut self, library_dir: &Path) -> Option<PathBuf> {
        match self.write(library_dir) {
            Ok(path) => {
                self.error = None;
                self.saved_path = Some(path.clone());
                self.open = false;
                Some(path)
            }
            Err(e) => {
                self.error = Some(e.to_string());
                None
            }
        }
    }

    /// The `machine.toml` the last successful `submit` wrote.
    pub fn saved_path(&self) -> Option<&Path> {
        self.saved_path.as_deref()
    }

    fn write(&self, library_dir: &Path) -> std::io::Result<PathBuf> {
        if self.name.trim().is_empty() {
            return Err(std::io::Error::other("a name is required"));
        }
        // The one field with no default and no fallback: an MT-32
        // machine with no ROMs is a machine that fails to start, and
        // failing here says so while there is still a form to fix it in
        // (doc 20 §4).
        if self.music == Music::Mt32 && self.mt32_roms.trim().is_empty() {
            return Err(std::io::Error::other(
                "the Roland MT-32 needs a directory holding your own CM-32L ROM images",
            ));
        }
        if let Some(edit) = &self.editing {
            let bundle_path = edit.bundle_path.clone();
            if self.advanced {
                // Validate before writing: a bad hand-edit shouldn't
                // silently corrupt the library with an unreadable bundle.
                toml::from_str::<Machine>(&self.advanced_toml).map_err(std::io::Error::other)?;
                std::fs::write(&bundle_path, &self.advanced_toml)?;
                return Ok(bundle_path);
            }
            self.build_machine(PathBuf::from(&self.disk_path)).save(&bundle_path)?;
            return Ok(bundle_path);
        }
        let dir = library::reserve_dir(library_dir, &self.name)?;
        let bundle_path = dir.join(library::BUNDLE_FILE);
        if self.advanced {
            toml::from_str::<Machine>(&self.advanced_toml).map_err(std::io::Error::other)?;
            std::fs::write(&bundle_path, &self.advanced_toml)?;
            return Ok(bundle_path);
        }
        let disk_path = if self.existing_disk {
            PathBuf::from(&self.disk_path)
        } else {
            let disk_path = dir.join("disk.qcow2");
            player::create_disk(&disk_path, self.disk_size_gb)?;
            disk_path
        };
        self.build_machine(disk_path).save(&bundle_path)?;
        Ok(bundle_path)
    }
}
