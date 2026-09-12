//! The machine bundle format (doc 07): a declarative `machine.toml` the
//! launcher reads and writes. "Hand-written bundles + the player binary is
//! a fully supported path" (doc 07) — `qemu_args` is the one place that
//! translates a bundle into a real `qemu-system-i386` command line; no
//! user-visible QEMU command line exists anywhere else.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Which reference guest configuration (doc 06) a machine is modeled on.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Family {
    Win98,
    Xp,
    /// A DOS machine: MS-DOS or FreeDOS on the same i440FX PC, with the
    /// SB16 the Win98 family already carries "for DOS boxes/games" (doc
    /// 06) and no network card. What actually makes it a *DOS* machine
    /// is `CpuSpeed`: the era's software paces itself by how fast the
    /// CPU is, and emulation is far too fast for it (doc 06).
    Dos,
    /// Anything else of the era on the same PC: BeOS, a period Linux,
    /// OS/2. Defined by what it *doesn't* get — our own paravirtual
    /// adapter (`d3dpt-vga`) is a Windows display driver and the 3D
    /// pass-through is a set of Windows DLLs, so none of it is reachable
    /// here. What is left is hardware every one of these systems shipped
    /// a driver for in the nineties: the Bochs/standard VGA with VBE 2.0,
    /// an RTL8139 and an ES1370. 2D, the CRT shader chain and the real
    /// CD-ROM model, which is the DOS family's story on a guest modern
    /// enough to want PCI cards.
    Other,
}

impl Family {
    /// In the order a picker should offer them: the three the project is
    /// actually built around first, then the catch-all.
    pub const ALL: [Family; 4] = [Family::Win98, Family::Xp, Family::Dos, Family::Other];

    pub fn label(self) -> &'static str {
        match self {
            Family::Win98 => "Win98",
            Family::Xp => "XP",
            Family::Dos => "DOS",
            Family::Other => "Other (BeOS, Linux, …)",
        }
    }
}

/// How fast the guest's CPU is allowed to be, named after the machine it
/// feels like rather than after the knob underneath.
///
/// DOS-era software calibrates delay loops against the CPU it finds and
/// then trusts the answer forever, so on a fast machine it does not merely
/// run quickly — it runs *wrong*: unplayable games, Turbo Pascal's
/// "runtime error 200", music that plays at double speed. Our TCG runs a
/// DOS guest at around 610 million instructions/s on the Linux box
/// (measured 2026-09-06, a tight loop under `-cpu pentium3`), which is
/// Pentium III territory; KVM is far beyond that.
///
/// QEMU's only rate control is `-icount`, whose `shift` sets one
/// instruction per 2^shift ns — so the rates below are powers of two by
/// construction, and the labels say which real machine each is closest
/// to. Two things follow from how it works, both of which the UI says
/// out loud: it needs `align=on` to pace against the host at all (without
/// it the guest only *believes* it is slow), and it cannot coexist with
/// KVM, so a throttled machine runs emulated whatever its `accel` says.
///
/// The cap is exact where it matters. Measured on the Linux box with a
/// 100M-instruction loop: 30.6 MIPS asked 31.25, 7.9 asked 7.8. Above
/// ~30 MIPS the alignment only corrects the guest when it falls *behind*,
/// so the fast entries are a ceiling the host may not reach and may
/// overshoot — which is why the two slowest entries are the ones a 1993
/// game should be given.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum CpuSpeed {
    /// No throttle: as fast as this host emulates (or KVM, if the machine
    /// asked for it). Right for Windows, wrong for most DOS software.
    #[default]
    #[serde(rename = "unthrottled")]
    Unthrottled,
    #[serde(rename = "pentium-133")]
    Pentium133,
    #[serde(rename = "pentium-75")]
    Pentium75,
    #[serde(rename = "486dx2-66")]
    Dx266,
    #[serde(rename = "486sx-25")]
    Sx25,
    #[serde(rename = "386dx-33")]
    Dx33,
    #[serde(rename = "286-12")]
    At286,
}

impl CpuSpeed {
    /// In the order a combo box should offer them: fastest first, because
    /// "as fast as possible" is the answer for everything that is not a
    /// DOS game, and the list then reads downwards through the eras.
    pub const ALL: [CpuSpeed; 7] = [
        CpuSpeed::Unthrottled,
        CpuSpeed::Pentium133,
        CpuSpeed::Pentium75,
        CpuSpeed::Dx266,
        CpuSpeed::Sx25,
        CpuSpeed::Dx33,
        CpuSpeed::At286,
    ];

    /// `-icount shift=`, or `None` for no throttle at all.
    pub fn icount_shift(self) -> Option<u32> {
        match self {
            CpuSpeed::Unthrottled => None,
            CpuSpeed::Pentium133 => Some(3), // 125 M instructions/s
            CpuSpeed::Pentium75 => Some(4),  // 62.5
            CpuSpeed::Dx266 => Some(5),      // 31.25
            CpuSpeed::Sx25 => Some(6),       // 15.6
            CpuSpeed::Dx33 => Some(7),       // 7.8
            CpuSpeed::At286 => Some(8),      // 3.9
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            CpuSpeed::Unthrottled => "Unthrottled (as fast as the host emulates)",
            CpuSpeed::Pentium133 => "Pentium 133 (~125 M instructions/s)",
            CpuSpeed::Pentium75 => "Pentium 75 (~62 M)",
            CpuSpeed::Dx266 => "486DX2-66 (~31 M)",
            CpuSpeed::Sx25 => "486SX-25 (~16 M)",
            CpuSpeed::Dx33 => "386DX-33 (~8 M)",
            CpuSpeed::At286 => "286-12 (~4 M)",
        }
    }
}

/// The display adapter, on the families that have a choice of one.
///
/// The choice exists because there are two honest answers and nothing
/// here can pick between them. On Windows it is *our* adapter and driver
/// (docs 15, 19) against the in-box driver Windows already has: ours is
/// what the whole display path is built on — the mode table, the linear
/// frame buffer the player scans out, the page flips that pace a game,
/// the Direct3D DDI — and the Cirrus is what a machine falls back to
/// when the driver is not installed yet, when it is being A/B'd against,
/// or when a title misbehaves on it. On `Other` there is no driver of
/// ours at all, so it is one standard adapter against another and only
/// the person installing the guest knows which has a driver in the box.
///
/// Every entry is an adapter with a real VGA BIOS, so any of them boots
/// anything; the difference is what the guest finds a driver for.
/// Which entries a family offers is `video_choices`, and the **first of
/// them is that family's default**.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Video {
    /// `d3dpt-vga`, our own paravirtual adapter, driven by our display
    /// driver from the guest-tools ISO (docs 15, 19). Windows only —
    /// there is no driver for it anywhere else, and a guest without one
    /// comes up on the plain VGA the device also is.
    #[serde(rename = "d3dpt")]
    D3dpt,
    /// QEMU's standard VGA: the Bochs adapter, VBE 2.0 and a linear
    /// frame buffer. What a period VESA driver wants, what a modern
    /// Linux binds `bochs-drm` to, and the later of the two VESA BIOSes
    /// a DOS title can find. **No XP driver at all** (XP falls back
    /// to 800×600×4 vga.sys), which is why the Windows families do not
    /// offer it.
    Std,
    /// Cirrus Logic GD5446. A chip that really existed, so a guest of
    /// the era is likely to have a *native* driver for it: Windows 98 and
    /// XP both have one in the box, and so do BeOS R5 and XFree86.
    Cirrus,
}

impl Video {
    /// Every variant, for serde round-trips and label lookups. **Not what
    /// a picker offers** — that is `video_choices(family)`, because half
    /// of these are wrong on any given family.
    pub const ALL: [Video; 3] = [Video::D3dpt, Video::Std, Video::Cirrus];

    pub fn label(self) -> &'static str {
        match self {
            Video::D3dpt => "Our own adapter (d3dpt-vga)",
            Video::Std => "Standard VGA (Bochs, VBE 2.0)",
            Video::Cirrus => "Cirrus Logic GD5446",
        }
    }

    /// The arguments that put this adapter on the machine. Our own is a
    /// `-device` on a machine with no `-vga` at all, and it carries the
    /// same PCI address the plain `-vga` adapters are given by the
    /// machine itself (0x02, measured), so the cards pinned below it do
    /// not move when the adapter is changed under an installed guest.
    fn args(self) -> [&'static str; 2] {
        match self {
            Video::D3dpt => ["-device", "d3dpt-vga,addr=0x02"],
            Video::Std => ["-vga", "std"],
            Video::Cirrus => ["-vga", "cirrus"],
        }
    }
}

/// The adapters a family offers, **first one its default**, or empty
/// where there is nothing to choose.
///
/// The Windows families choose between our adapter and the one Windows
/// has an in-box driver for; they are not offered the standard VGA,
/// which has no XP driver at all. They start on opposite ends of that
/// pair: XP on ours, Win98 on the Cirrus. `Other` chooses between the two
/// standard adapters, since nothing of ours runs there. DOS chooses
/// between those same two, and for the one reason that has nothing to do
/// with drivers: its titles program the adapter themselves, so what
/// changes is **which VESA BIOS the game finds** — the Bochs one's VBE
/// 2.0 with its linear frame buffer, or the Cirrus's of the period.
pub fn video_choices(family: Family) -> &'static [Video] {
    match family {
        // XP starts on ours: the driver has been the whole display path
        // there since 2026-09-04 and every game the M4/M7 tracks were
        // built on runs through it.
        Family::Xp => &[Video::D3dpt, Video::Cirrus],
        // Win98 starts on the Cirrus (2026-09-07). Ours runs there too
        // (doc 19) and is one pick away, but the 9x driver is a day old
        // against XP's, so a machine the wizard makes comes up on the
        // driver Windows already has in the box and moves to ours when
        // whoever installed the guest decides to.
        Family::Win98 => &[Video::Cirrus, Video::D3dpt],
        Family::Other => &[Video::Std, Video::Cirrus],
        // DOS starts on the standard VGA (2026-09-09, user decision):
        // its VBE 2.0 and linear frame buffer are the fuller of the two
        // VESA BIOSes a title can find, and the Cirrus — which is what a
        // DOS machine got while the adapter was hardcoded, and what
        // `Other` is offered for its *native* drivers — is the other
        // half of an A/B nothing else here can settle: a title whose
        // modes come out wrong on one BIOS is the only evidence there
        // is. Our own adapter is not on offer, there being no DOS driver
        // for it anywhere.
        Family::Dos => &[Video::Std, Video::Cirrus],
    }
}

/// What a host gamepad does for this machine (M13,
/// `docs/tracks/m13-gamepads.md`).
///
/// All three guest-facing paths exist now: a USB HID gamepad (patch 26)
/// for the families with a USB stack, a gameport at 0x201 (patch 27) for
/// the ones without, and the key mapping for everything else. Which of
/// them a family is *offered* is `pad_choices`, and it is offered only
/// where this project can say what the guest needs — the same rule the
/// display-adapter picker follows for an adapter a family has no driver
/// for.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Pad {
    /// A controller plugged into the host does nothing. The default, and
    /// not merely the conservative choice: with `Keys` a resting stick
    /// that drifts past the threshold holds an arrow key down, and on a
    /// desktop that is a cursor sliding across the screen with no
    /// visible cause. Someone who wants a pad says so.
    #[default]
    None,
    /// A real USB HID gamepad on the machine (`-usb -device
    /// usb-gamepad`, patch 26). Two sticks, an 8-way hat and twelve
    /// buttons, bound by the guest's own in-box HID stack — DirectInput
    /// and `joy.cpl` see it on the first boot after it is added. This is
    /// the entry a game of the era can actually use: it enumerates as a
    /// controller, and the sticks are analog.
    ///
    /// **Confirmed with a real controller on 2026-09-09**, on XP and on
    /// Windows 98 SE, both showing it in the Game Controllers panel. The
    /// two are not the same experience and the wizard says so: XP needs
    /// nothing, and 98 SE binds its own driver but asks for the Windows
    /// 98 source files the first time — the CD, or the CAB folder on the
    /// disk. Saying "nothing to install" for both, as this doc did until
    /// that run, leaves someone staring at a file-copy dialog wondering
    /// what went wrong.
    ///
    /// Not offered on DOS, which has no USB stack at all — that is what
    /// path B's gameport is for. Windows 98 *first edition* is still the
    /// doubt on the 9x side: its USB support predates the HID class being
    /// reliable, and it may want the USB supplement. Untried.
    Usb,
    /// The analog joystick port at 0x200-0x207 (`-device gameport`, patch
    /// 27): four one-shots and four buttons, which is the whole of what
    /// the hardware ever had. The **only** path that reaches DOS, where a
    /// game reads the port itself and there is no USB stack for path A to
    /// use — and the period-correct one, since this is the connector the
    /// sticks of the era plugged into.
    ///
    /// What the guest has to do is not nothing, and the wizard says so:
    /// the port is not Plug and Play (it never was), so Windows 9x wants
    /// Add New Hardware and then a calibration pass in the Game
    /// Controllers panel. DOS needs neither.
    ///
    /// Nobody here has done that, and the wizard steers away from it
    /// rather than describing it as the way: on 98 a *Windows* game gets
    /// its joystick from [`Pad::Usb`] through both APIs one can call —
    /// DirectInput and winmm's `joyGetPosEx` on top of VJOYD, measured by
    /// the `pad-guest-98` check — so the driver half of this port was
    /// dropped from M13 rather than built. What is left to this variant
    /// is what it was built for: DOS, which has no USB stack, and a DOS
    /// box under Windows 98, which reads 0x201 itself.
    ///
    /// Not offered on XP: `gameenum.sys` is still in the box, but a
    /// non-PnP port has nothing to enumerate it and Microsoft was already
    /// retiring analog sticks — XP's answer is path A. Nor on `Other`,
    /// where the guest is an OS this project cannot name a driver step
    /// for.
    Gameport,
    /// The pad presses keys: the player maps its controls onto the key
    /// events it already sends, against `gamepad::default_key_bindings`.
    /// Reaches **every** guest — DOS, Win98 FE, XP, `Other` — because
    /// there is no device for the guest to support. The cost is that it
    /// is a mapping and not a controller: no analog anything, and a game
    /// that enumerates DirectInput or reads 0x201 still finds nothing.
    Keys,
}

impl Pad {
    pub const ALL: [Pad; 4] = [Pad::None, Pad::Usb, Pad::Gameport, Pad::Keys];

    pub fn label(self) -> &'static str {
        match self {
            Pad::None => "No gamepad",
            Pad::Usb => "USB gamepad",
            Pad::Gameport => "Gameport joystick",
            Pad::Keys => "Gamepad presses keys",
        }
    }

    /// The name this serializes to. Must agree with the `rename_all`
    /// above — `pad_lenient` reads through this, so a disagreement would
    /// make every bundle's `pad` field fall back to the default in
    /// silence. The `pad` check in `scripts/test.sh` writes a machine
    /// and reads it back to prove they still agree.
    pub fn name(self) -> &'static str {
        match self {
            Pad::None => "none",
            Pad::Usb => "usb",
            Pad::Gameport => "gameport",
            Pad::Keys => "keys",
        }
    }

    pub fn from_name(name: &str) -> Option<Pad> {
        Pad::ALL.into_iter().find(|p| p.name() == name)
    }
}

/// Reads `pad` **leniently**: a value this build has never heard of
/// becomes `None` — the family default — instead of failing the whole
/// bundle.
///
/// Unlike every other enum in this file, `Pad` was *known* to be gaining
/// variants — paths A and B of the M13 track added `usb` and `gameport`
/// after the field shipped — and the rule stays now that they have: a
/// machine someone made in a newer build and opened in an older one would
/// otherwise refuse to load at all, not "the pad setting was ignored" but
/// "this machine does not exist", losing its disk, its discs and its
/// shader profile over a field about a controller. That trade is never
/// worth it, so this one field is read the forgiving way.
fn pad_lenient<'de, D>(d: D) -> Result<Option<Pad>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    Ok(Option::<String>::deserialize(d)?.and_then(|s| Pad::from_name(&s)))
}

/// What each family offers, **first one its default**.
///
/// The two asymmetries are the two devices, and each is a driver
/// question rather than a preference. `Usb` needs a guest with a USB
/// stack, which DOS has not got at all. `Gameport` needs a guest that
/// will talk to a port nothing enumerates: DOS reads it directly and 9x
/// has "Standard Game Port" for it, while XP would need a driver
/// installed by hand for a class of device it was already dropping, and
/// the `Other` family is an OS this project cannot name a step for. So
/// DOS is offered the gameport and not the HID pad, XP and `Other` the
/// HID pad and not the gameport, and Win98 — which has both stacks —
/// both.
///
/// Every family starts on `None`, and that is a decision rather than
/// caution. A machine nobody asked for a pad on should not grow a device
/// in its Device Manager, and it is the same call this project already
/// made for networking, which is off on new machines because a guest that
/// waits on DHCP at boot is worse than one with no network. A pad is one
/// pick away either way.
pub fn pad_choices(family: Family) -> &'static [Pad] {
    match family {
        // No USB stack, so no HID gamepad. Nothing to warn about — the
        // entry simply is not offered, the way the display-adapter picker
        // does not offer an adapter a family has no driver for.
        Family::Dos => &[Pad::None, Pad::Gameport, Pad::Keys],
        Family::Win98 => &[Pad::None, Pad::Usb, Pad::Gameport, Pad::Keys],
        Family::Xp | Family::Other => &[Pad::None, Pad::Usb, Pad::Keys],
    }
}

/// The pad setting a family starts on. Always the first of
/// `pad_choices`, so the list and the default cannot disagree.
pub fn default_pad(family: Family) -> Pad {
    pad_choices(family).first().copied().unwrap_or(Pad::None)
}

/// The digital sound card, and — for the two cards that carried one —
/// the FM chip that comes with it (doc 20 §6).
///
/// The era's split is the reason this is a choice at all: a Sound
/// Blaster is what a DOS game knows how to find and what Windows 98 has
/// a driver for in the box, and an AC'97 is what a machine of 2001 has
/// and what sounds better. Neither is right for both families.
///
/// **The FM chip is not in this list, on purpose.** A card either had
/// one or it did not: an SB16 carries a YMF262 at 0x388 and at its own
/// base, an AdLib *is* one, and an AC'97, an ES1370 and a Gravis have
/// none. So picking a card here decides whether the machine has FM, the
/// way buying one did — and a game that only knows AdLib music finds it
/// on exactly the machines where it would have.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Sound {
    /// Sound Blaster 16 at 0x220 (IRQ 5, DMA 1/5) **and its OPL3**.
    /// What `BLASTER=A220 I5 D1 H5 T6` describes, and the card every
    /// DOS title of the CD-ROM era has a driver for.
    Sb16,
    /// The Intel AC'97 codec: 2001's card, and XP's in-box driver.
    /// 98 has a driver for it in the guest tools (doc 06), not in the
    /// box. No FM at all — a DOS box inside such a machine has no music.
    Ac97,
    /// Ensoniq AudioPCI (ES1370): doc 06's card for the `Other`
    /// family, the one BeOS R5 and a period Linux both drive in the box.
    Es1370,
    /// Gravis Ultrasound: a wavetable card, so its *music* is its own —
    /// no MPU-401 and no FM involved — and the guest needs Gravis's own
    /// drivers and its `ULTRASND` line before anything comes out of it.
    Gus,
    /// An AdLib and nothing else: the OPL3 at 0x388, no digital audio at
    /// all. The 1990 machine, for a title that predates sampled sound.
    Adlib,
    /// No sound card. The machine keeps whatever the *music* picker
    /// gives it, which is a real configuration: an MPU-401 and a module
    /// was how music was done before cards could play samples.
    None,
}

impl Sound {
    /// Every variant, for serde round-trips and label lookups. **Not
    /// what a picker offers** — that is `sound_choices(family)`.
    pub const ALL: [Sound; 6] = [Sound::Sb16, Sound::Ac97, Sound::Es1370, Sound::Gus, Sound::Adlib, Sound::None];

    pub fn label(self) -> &'static str {
        match self {
            Sound::Sb16 => "Sound Blaster 16 (with FM)",
            Sound::Ac97 => "Intel AC'97",
            Sound::Es1370 => "Ensoniq AudioPCI (ES1370)",
            Sound::Gus => "Gravis Ultrasound",
            Sound::Adlib => "AdLib (FM only)",
            Sound::None => "No sound card",
        }
    }

    /// The name a bundle and the debug verbs use.
    pub fn key(self) -> &'static str {
        match self {
            Sound::Sb16 => "sb16",
            Sound::Ac97 => "ac97",
            Sound::Es1370 => "es1370",
            Sound::Gus => "gus",
            Sound::Adlib => "adlib",
            Sound::None => "none",
        }
    }

    /// The devices this card is. The PCI cards carry their address for
    /// the reason every pinned address here exists: removing the NIC
    /// above them must not slide them into its slot, which an installed
    /// guest would see as its sound card having been swapped.
    fn args(self) -> Vec<String> {
        let device = |spec: &str| vec!["-device".to_string(), spec.to_string()];
        match self {
            // Two devices, because the card is two chips: the SB16 for
            // digital audio and the OPL3 that sits at 0x388 *and* is
            // mirrored at the card's own base, which is where an
            // SB-aware driver looks for it.
            Sound::Sb16 => {
                let mut v = device("sb16,audiodev=embed0");
                v.extend(device("opl3,audiodev=embed0,sbbase=0x220"));
                v
            }
            Sound::Adlib => device("opl3,audiodev=embed0"),
            Sound::Ac97 => device("AC97,audiodev=embed0,addr=0x04"),
            Sound::Es1370 => device("ES1370,audiodev=embed0,addr=0x04"),
            Sound::Gus => device("gus,audiodev=embed0"),
            Sound::None => Vec::new(),
        }
    }
}

/// What is behind the machine's MIDI port (doc 20 §6): an MPU-401 at
/// 0x330 and the synthesizer that plays what the guest writes to it.
///
/// This is the *music* half of a period machine's audio, and the half
/// QEMU never had — there is no MPU-401 device in it at all, so a game
/// offering "General MIDI" or "Roland MT-32" in its setup program had
/// nothing to talk to here.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Music {
    /// A SoundFont General MIDI synthesizer. The bank we ship unless
    /// `soundfont` names another, and which bank it is matters more to
    /// how the music sounds than anything else on this screen.
    Gm,
    /// A Roland CM-32L — the MT-32 family — which needs the user's own
    /// ROM images in `mt32_roms`. What a 1990 title means by "Roland".
    Mt32,
    /// No MIDI port on the machine at all. Deliberately not "a port
    /// that swallows notes": a game that found one would pick it and
    /// play to nobody, which is worse than not offering it.
    None,
}

impl Music {
    pub const ALL: [Music; 3] = [Music::Gm, Music::Mt32, Music::None];

    pub fn label(self) -> &'static str {
        match self {
            Music::Gm => "General MIDI (SoundFont)",
            Music::Mt32 => "Roland MT-32 / CM-32L",
            Music::None => "No MIDI port",
        }
    }

    pub fn key(self) -> &'static str {
        match self {
            Music::Gm => "gm",
            Music::Mt32 => "mt32",
            Music::None => "none",
        }
    }
}

/// The cards a family offers, **first one its default**.
///
/// Every family keeps the card it already had as its first entry, so no
/// existing machine changes hardware by being opened: 98 and DOS on the
/// Sound Blaster, XP on the AC'97, `Other` on the Ensoniq. What is new
/// is that the others are reachable — an AC'97 in a 98 machine that
/// wants the better codec, a Gravis in a DOS machine for the games
/// written for one, an AdLib for a 1990 title, and nothing at all.
pub fn sound_choices(family: Family) -> &'static [Sound] {
    match family {
        // Windows has the SB16 driver in the box and a DOS box inside 98
        // finds the card it expects; the AC'97 needs the guest-tools
        // driver (doc 06) and gives the machine no FM.
        Family::Win98 => &[Sound::Sb16, Sound::Ac97, Sound::Gus, Sound::None],
        Family::Dos => &[Sound::Sb16, Sound::Gus, Sound::Adlib, Sound::None],
        // XP's own driver, and the card doc 06 has always given it.
        Family::Xp => &[Sound::Ac97, Sound::Sb16, Sound::None],
        // Standard hardware only, as everywhere else in this family:
        // both of these had a driver in the box on BeOS R5 and on a
        // period Linux, where nothing of ours can be installed after.
        Family::Other => &[Sound::Es1370, Sound::Ac97, Sound::None],
    }
}

/// What is on the MIDI port, **first one the family's default**.
///
/// The two families with no synthesizer of their own get one: a DOS
/// machine has nothing but the card's FM otherwise, and Windows 98's
/// own MIDI output is that same FM chip. XP ships a wavetable
/// synthesizer with the operating system and `Other` is a family we add
/// no drivers to, so both start with no port — it is one pick away when
/// a game wants a real MT-32.
pub fn music_choices(family: Family) -> &'static [Music] {
    match family {
        Family::Win98 | Family::Dos => &[Music::Gm, Music::Mt32, Music::None],
        Family::Xp | Family::Other => &[Music::None, Music::Gm, Music::Mt32],
    }
}

/// The card a family starts on. Always the first of `sound_choices`.
pub fn default_sound(family: Family) -> Sound {
    sound_choices(family).first().copied().unwrap_or(Sound::None)
}

/// What a family's MIDI port starts as. Always the first of
/// `music_choices`.
pub fn default_music(family: Family) -> Music {
    music_choices(family).first().copied().unwrap_or(Music::None)
}

/// Which drive the machine boots from. `Auto` leaves the order to QEMU,
/// which tries the hard disk, then the floppy, then the CD — the right
/// answer for an installed Windows and for the wizard's "boot the
/// installer from the CD because the new disk is blank" case alike.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Boot {
    #[default]
    Auto,
    Disk,
    /// The floppy first, the disk if there is no disk in it: a DOS boot
    /// disk, or a Windows machine being repaired from one.
    Floppy,
    /// The CD first: reinstalling over a disk that still boots.
    Cd,
}

impl Boot {
    pub const ALL: [Boot; 4] = [Boot::Auto, Boot::Disk, Boot::Floppy, Boot::Cd];

    pub fn label(self) -> &'static str {
        match self {
            Boot::Auto => "Automatic",
            Boot::Disk => "Hard disk",
            Boot::Floppy => "Floppy, then hard disk",
            Boot::Cd => "CD, then hard disk",
        }
    }

    fn order(self) -> Option<&'static str> {
        match self {
            Boot::Auto => None,
            Boot::Disk => Some("c"),
            Boot::Floppy => Some("ac"),
            Boot::Cd => Some("dc"),
        }
    }
}

/// How the guest's instructions are executed. Kept in the bundle rather
/// than decided at spawn time, because it is a property of the machine a
/// user can want to pin: an era CPU under TCG is the reference behaviour
/// the whole project is tuned for (docs 13 and 16's x87/SSE fast paths
/// only exist there), while KVM is what makes an XP game playable on a
/// Linux host.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Accel {
    /// Hardware acceleration when the host has it, emulation otherwise.
    /// QEMU itself picks, from the `kvm:tcg` (Windows: `whpx:tcg`) list —
    /// no host probing here can be wrong.
    #[default]
    Auto,
    /// Hardware acceleration only: the machine refuses to start without
    /// it, which is what "required" has to mean to be worth choosing over
    /// `Auto`. Named `kvm` in the bundle for every host — the field says
    /// what the user asked for, and each host spells it its own way
    /// (`whpx` on Windows), so a machine directory copied between them
    /// keeps meaning the same thing.
    Kvm,
    /// Emulation only. The honest choice for Win98: KVM runs the guest at
    /// host speed, and Win9x has real fast-CPU bugs (doc 06) that the
    /// `pentium3` model does not protect against, since it is the *speed*
    /// that trips them.
    Tcg,
}

impl Accel {
    pub const ALL: [Accel; 3] = [Accel::Auto, Accel::Kvm, Accel::Tcg];

    /// The label both front ends show. Here rather than inline in a
    /// combo box, like `Family::label` and `Boot::label`, so the two
    /// cannot end up offering differently-worded choices.
    pub fn label(self) -> &'static str {
        match self {
            Accel::Auto => "Automatic",
            // Named for what this host actually has — the same setting,
            // spelled the way the machine in front of the user spells it
            // (`crate::player::hw_accel_label`).
            Accel::Kvm if cfg!(target_os = "windows") => "WHPX (required)",
            Accel::Kvm if cfg!(target_os = "linux") => "KVM (required)",
            Accel::Kvm => "Hardware acceleration (required)",
            Accel::Tcg => "Emulation",
        }
    }
}

/// One of the QEMU fast paths this project carries as its own patches
/// (`patches/qemu/README.md`, docs 13 and 16, the M8/M9 tracks), with
/// the off switch the patch already gave it.
///
/// Every one of them is *the emulator running the guest's arithmetic on
/// the host's own silicon instead of simulating it*, which is why each
/// arrived with an off switch: the switch is the oracle. When a guest
/// computes the wrong number or a game stops drawing, one run with one
/// of these off says whether a fast path did it — the alternative is
/// bisecting a patch queue against a Windows install.
///
/// They therefore only exist under emulation. A machine running on KVM
/// executes on the host CPU directly and none of these is reachable;
/// the form says so rather than showing eight switches that do nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Optimization {
    X87Fast,
    SseFast,
    SimdFast,
    RepFast,
    SmcSameValue,
    SoftImm,
    InlineLookup,
    TbInvalidateFast,
    TlbFloor,
    TlsHotPaths,
    JumpCacheKeep,
    EobChain,
    TlbRetire,
    PinnedRegs,
}

/// Where an optimization's switch goes on the command line: a property
/// of the guest CPU (`-cpu pentium3,x87-fast=off`) or of the TCG
/// accelerator itself (`-accel tcg,smc-same-value=off`). The two are not
/// interchangeable — QEMU looks each name up on a different object — and
/// the accelerator half is the reason `qemu_args` spells the accelerator
/// as `-accel` rather than `-machine accel=`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Knob {
    Cpu,
    Tcg,
}

impl Optimization {
    /// In the order the form lists them: the arithmetic fast paths
    /// first, in the order they were written, then the two that are
    /// about translation, then the experimental one.
    pub const ALL: [Optimization; 14] = [
        Optimization::X87Fast,
        Optimization::SseFast,
        Optimization::SimdFast,
        Optimization::RepFast,
        Optimization::SmcSameValue,
        Optimization::SoftImm,
        Optimization::InlineLookup,
        Optimization::TbInvalidateFast,
        Optimization::TlbFloor,
        Optimization::TlsHotPaths,
        Optimization::JumpCacheKeep,
        Optimization::EobChain,
        Optimization::TlbRetire,
        Optimization::PinnedRegs,
    ];

    /// The QEMU property name, which is also the key in `machine.toml`:
    /// one name for the thing, so a bundle can be read against
    /// `patches/qemu/README.md` without a translation table.
    pub fn key(self) -> &'static str {
        match self {
            Optimization::X87Fast => "x87-fast",
            Optimization::SseFast => "sse-fast",
            Optimization::SimdFast => "simd-fast",
            Optimization::RepFast => "rep-fast",
            Optimization::SmcSameValue => "smc-same-value",
            Optimization::SoftImm => "soft-imm",
            Optimization::InlineLookup => "inline-lookup",
            Optimization::TbInvalidateFast => "tb-invalidate-fast",
            Optimization::TlbFloor => "tlb-floor",
            Optimization::TlsHotPaths => "tls-hot-paths",
            Optimization::JumpCacheKeep => "jump-cache-keep",
            Optimization::EobChain => "eob-chain",
            Optimization::TlbRetire => "tlb-retire",
            Optimization::PinnedRegs => "pinned-regs",
        }
    }

    fn knob(self) -> Knob {
        match self {
            Optimization::X87Fast
            | Optimization::SseFast
            | Optimization::SimdFast
            | Optimization::RepFast => Knob::Cpu,
            Optimization::SmcSameValue
            | Optimization::SoftImm
            | Optimization::InlineLookup
            | Optimization::TbInvalidateFast
            | Optimization::TlbFloor
            | Optimization::TlsHotPaths
            | Optimization::JumpCacheKeep
            | Optimization::EobChain
            | Optimization::TlbRetire
            | Optimization::PinnedRegs => Knob::Tcg,
        }
    }

    /// Whether a machine that says nothing has it on. Everything that
    /// has shipped is on — turning one off is a diagnosis, not a
    /// preference — and `pinned-regs` is off because the patch itself is
    /// off by default while the work is in progress.
    pub fn default_on(self) -> bool {
        self != Optimization::PinnedRegs
    }

    /// The checkbox's label: what the fast path does, not what it is
    /// called in the patch queue.
    pub fn label(self) -> &'static str {
        match self {
            Optimization::X87Fast => "x87 floating point on the host FPU",
            Optimization::SseFast => "SSE floating point on the host",
            Optimization::SimdFast => "MMX and SSE integer instructions inline",
            Optimization::RepFast => "Block string moves as whole-page copies",
            Optimization::SmcSameValue => "Skip retranslation when code is rewritten unchanged",
            Optimization::SoftImm => "Read patched operands from the guest's code as it runs",
            Optimization::InlineLookup => "Find the next block without leaving generated code",
            Optimization::TbInvalidateFast => "Skip the block walk for writes that can't hit code",
            Optimization::TlbFloor => "Keep the address-translation cache from shrinking",
            Optimization::TlsHotPaths => "Take the memory-tracking locks once per run, not per write",
            Optimization::JumpCacheKeep => "Keep the block cache across the guest's context switches",
            Optimization::EobChain => "Chain past segment loads, sti and popf when no interrupt waits",
            Optimization::TlbRetire => "Keep the address cache across the guest's own TLB flushes",
            Optimization::PinnedRegs => "Keep guest registers in host registers (experimental)",
        }
    }

    /// The sentence under it: what it buys, and — for the one case where
    /// it matters — why it is off.
    pub fn note(self) -> &'static str {
        match self {
            Optimization::X87Fast => {
                "Windows and Direct3D run the x87 unit at a precision the host reproduces exactly, \
                 so the guest's floating point is executed rather than simulated. Super PI 1M on the \
                 M1 Air: 9:49 off, 1:57 on. Turn it off if a guest's arithmetic looks wrong."
            }
            Optimization::SseFast => {
                "Packed and scalar SSE/SSE2 arithmetic on the host's vector unit instead of a call \
                 per instruction: 7-12x on packed code, 3-4x on scalar."
            }
            Optimization::SimdFast => {
                "MMX and SSE integer maths, shuffles and packs become host vector instructions: \
                 2-4x on the pixel loops of an era software renderer."
            }
            Optimization::RepFast => {
                "REP MOVS / STOS copies a page at a time through memcpy instead of one element per \
                 loop - 30x on the blits an era game fills the screen with."
            }
            Optimization::SmcSameValue => {
                "Self-modifying code usually writes back the bytes already there, and rewriting a \
                 value with itself cannot invalidate anything. Moto Racer's race: 7.3 to 21.7 fps."
            }
            Optimization::SoftImm => {
                "A game that patches the operands of its own inner loop -- every software renderer \
                 of the era does -- has them read from its code as it runs, so the patch costs \
                 nothing instead of a retranslation. Moto Racer's race: 41 to 58 fps."
            }
            Optimization::InlineLookup => {
                "Every return and indirect jump finds its next block in generated code rather than \
                 through a helper call: 7-Zip in the guest, +7-12%."
            }
            Optimization::TbInvalidateFast => {
                "A guest write into a page that holds code used to walk every block on it. Each page \
                 now remembers where its code actually lies, so a write outside that range returns at \
                 once. Found on a 1997 game whose data pages carried one stale block and were written \
                 tens of thousands of times a second."
            }
            Optimization::TlbFloor => {
                "The emulator's address-translation cache is resized at every flush, and a Windows \
                 guest flushes at every context switch - which shrank it to 64 entries, where two live \
                 pages collide constantly. It is held at 4096 instead."
            }
            Optimization::TlsHotPaths => {
                "The bookkeeping that tracks which guest memory has changed took a lock per access; \
                 it now runs under the one the emulator already holds for the whole run. On macOS each \
                 of those was a call into the dynamic linker: 8% of a game's emulation thread."
            }
            Optimization::JumpCacheKeep => {
                "The cache that finds the next block after a return or an indirect jump was emptied \
                 at every context switch, and Windows 98 makes thousands a second: every entry is \
                 kept and checked against the page it came from instead. 3DMark 99's first-person \
                 test on the Ryzen: 18.2 to 19.0 fps."
            }
            Optimization::EobChain => {
                "A block that ended after a segment-register load, an sti, a popf or a control-word \
                 change used to return to the emulator's main loop every time; it now finds the next \
                 block directly unless an interrupt is actually pending. Windows 98's ring-0 entry \
                 alone was five such round trips per system call."
            }
            Optimization::TlbRetire => {
                "Windows 98 reloads CR3 to flush its TLB after every page it maps or unmaps, thousands \
                 of times a second; the emulator used to throw its whole address cache away and walk \
                 the page tables again for every page. The flushed cache is kept, and an entry comes \
                 back once the page-table entries it was computed from are checked unchanged."
            }
            Optimization::PinnedRegs => {
                "Apple Silicon only, and still being worked on - a boot crash has been seen with it \
                 on. Leave it off unless you are testing it."
            }
        }
    }
}

/// A machine's optimization settings: **only what differs from
/// `Optimization::default_on` is stored**, so a bundle that says nothing
/// runs exactly the way every bundle written before this field existed
/// did, and an optimization added to the patch queue later arrives on in
/// every bundle that already exists.
///
/// Keyed by the QEMU property name rather than by the enum, so a bundle
/// written by a launcher that knows an optimization this build does not
/// survives a load and a save with the entry intact instead of being
/// refused as an unknown variant.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct Optimizations(BTreeMap<String, bool>);

impl Optimizations {
    pub fn enabled(&self, opt: Optimization) -> bool {
        self.0.get(opt.key()).copied().unwrap_or_else(|| opt.default_on())
    }

    pub fn is_default(&self, opt: Optimization) -> bool {
        self.enabled(opt) == opt.default_on()
    }

    /// Turn one on or off. Putting it back on its own default *removes*
    /// the entry rather than writing it out, which is what keeps a
    /// `machine.toml` holding only what someone actually changed.
    pub fn set(&mut self, opt: Optimization, on: bool) {
        if on == opt.default_on() {
            self.0.remove(opt.key());
        } else {
            self.0.insert(opt.key().to_string(), on);
        }
    }

    /// Every optimization back on its default. Only the ones this build
    /// knows about: an entry a newer launcher wrote is not something
    /// this one can decide is wrong.
    pub fn reset(&mut self) {
        for opt in Optimization::ALL {
            self.0.remove(opt.key());
        }
    }

    pub fn all_default(&self) -> bool {
        Optimization::ALL.iter().all(|opt| self.is_default(*opt))
    }

    /// Every optimization this build knows about turned **off** — the
    /// control run, in one click, for "is one of ours what broke this
    /// guest". Written as explicit `false` entries for the ones whose
    /// default is on, exactly as unticking each box would, so the
    /// bundle says what it means and `--print-args` shows the whole
    /// line. It is deliberately not the same shape as `reset`, which
    /// *removes* entries.
    ///
    /// **This is not a pristine QEMU.** Three patches of the queue have
    /// no runtime switch at all — 15 (`tb-invalidate-fast`), 16
    /// (`tlb-floor`) and 19 (`tls-hot-paths`) — so a guest that is still
    /// wrong with everything here off has not cleared our tree, only the
    /// eight switches. `Form::optimizations_note` says so where someone
    /// about to rely on it will read it.
    pub fn disable_all(&mut self) {
        for opt in Optimization::ALL {
            self.set(opt, false);
        }
    }

    /// Every optimization this build knows about turned **on**, the
    /// other end of the same shortcut. `pinned-regs` comes on with it:
    /// the switch means what it says, and its own note is where the
    /// warning about it lives.
    pub fn enable_all(&mut self) {
        for opt in Optimization::ALL {
            self.set(opt, true);
        }
    }

    /// Whether every optimization this build knows about is off, for a
    /// front end deciding whether its "Turn everything off" is worth
    /// offering.
    pub fn all_off(&self) -> bool {
        Optimization::ALL.iter().all(|opt| !self.enabled(*opt))
    }

    /// The same at the other end.
    pub fn all_on(&self) -> bool {
        Optimization::ALL.iter().all(|opt| self.enabled(*opt))
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// What a collapsed section says about itself, so someone who never
    /// opens it still sees that something in there has been changed.
    /// A count rather than "all on", because one of them ships off.
    pub fn summary(&self) -> String {
        let on = Optimization::ALL.iter().filter(|opt| self.enabled(**opt)).count();
        format!("{on} of {} on", Optimization::ALL.len())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Machine {
    pub name: String,
    pub family: Family,
    pub ram_mb: u32,
    /// How to execute the guest, or `None` for "whatever this family
    /// runs as" (`default_accel`). Absent rather than defaulted, so a
    /// bundle written before this field existed follows its family
    /// instead of silently acquiring KVM — which for a Win98 machine
    /// would be a *change* to how it had been running. Anything this
    /// launcher saves carries an explicit value: the form always has one.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub accel: Option<Accel>,
    /// Whether the machine has a network adapter at all (doc 06's
    /// per-family NIC on QEMU's user-mode NAT). `false` gives the guest
    /// no adapter rather than an unplugged one: "no networking" should
    /// mean Windows never sees a card, never asks for its driver and
    /// never waits on a network at boot.
    ///
    /// Defaults to `true` when the field is absent, which is how every
    /// bundle written before it existed ran — a machine must not lose
    /// its network by being read by a newer launcher.
    #[serde(default = "network_enabled_default")]
    pub network: bool,
    /// Whether the machine gets the USB tablet: an *absolute* pointing
    /// device, so the host pointer and the guest cursor are the same
    /// pointer and the window never has to grab anything (doc 03's
    /// pointer model, doc 06's "USB tablet optional"). `false` leaves
    /// the machine the PS/2 mouse the chipset already gives it, which is
    /// relative — the player then grabs on a click and Ctrl+Alt+G gives
    /// the pointer back, which is what mouselook needs and the only
    /// thing a DOS mouse driver can read.
    ///
    /// Defaults to `true` when the field is absent, which is how every
    /// bundle written before it existed ran — the tablet was
    /// unconditional, and a machine must not have its pointer change
    /// under it by being read by a newer launcher.
    #[serde(default = "seamless_mouse_default")]
    pub seamless_mouse: bool,
    /// Primary IDE hard disk (qcow2).
    pub disk: PathBuf,
    /// The disc in the CD-ROM drive when the machine boots, if any. Just
    /// one: the *collection* of discs is the shared shelf
    /// (`disc_library.rs`), not a per-machine list, and any other disc is
    /// swapped in at runtime through the monitor (`control.rs`).
    #[serde(default)]
    pub disc: Option<PathBuf>,
    /// Superseded by `disc` + the shared shelf. Read so bundles written
    /// before the shelf became shared still boot the disc they named
    /// (see `boot_disc`), and so their other entries can be imported
    /// (`DiscLibrary::import_legacy`); `save` drops it.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub discs: Vec<PathBuf>,
    /// A named shader profile (`shader_library`) to run this machine
    /// with, by id; `None` uses the app default. Takes precedence over
    /// `shader` when both are set.
    #[serde(default)]
    pub shader_profile: Option<String>,
    /// Raw shader preset override, bypassing the profile manager (doc 07
    /// settings taxonomy: the advanced/hand-written-bundle escape hatch).
    /// `None` uses the app default.
    #[serde(default)]
    pub shader: Option<PathBuf>,

    /// A floppy image in the machine's A: drive, if it has one. Doc 06
    /// lists a floppy on the Win98 machine ("driver/utility sneakernet,
    /// boot disks") and doc 07 lists floppy images among the media the
    /// launcher handles; a DOS machine may boot from one. Absent means
    /// no disk in the drive — the controller is there either way, as on
    /// a real PC of the era.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub floppy: Option<PathBuf>,

    /// Which drive to boot from. Absent = `Boot::Auto`, which is what
    /// every bundle written before this field existed was doing.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub boot: Option<Boot>,

    /// How fast the CPU is allowed to run (`CpuSpeed`). Absent =
    /// unthrottled, again what every earlier bundle was doing.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cpu_speed: Option<CpuSpeed>,

    /// The display adapter, on a family that has a choice of one
    /// (`default_video` — `Other` alone today). Absent = that family's
    /// default, and on every other family the field is not read at all:
    /// their adapter is what their driver is written for.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub video: Option<Video>,

    /// What a host gamepad does for this machine (`Pad`). Absent = that
    /// family's default, which is `Pad::None` everywhere — so a bundle
    /// written before this field existed keeps behaving exactly as it
    /// did, which for a gamepad means ignoring one.
    #[serde(
        default,
        deserialize_with = "pad_lenient",
        skip_serializing_if = "Option::is_none"
    )]
    pub pad: Option<Pad>,

    /// The sound card (`Sound`). Absent = the family's default, which
    /// is the card that family already had, so a bundle written before
    /// this field existed describes the same machine it always did.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sound: Option<Sound>,

    /// What is on the MIDI port (`Music`). Absent = the family's
    /// default: a General MIDI synthesizer on the two families with no
    /// synthesizer of their own, no port at all on the other two.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub music: Option<Music>,

    /// A SoundFont bank of the user's own, for `Music::Gm`. Absent =
    /// the one the package ships, which the *player* names to QEMU
    /// (`LIBSYNTH_SF2`, `player/src/companions.rs`) rather than the
    /// bundle: where an installed tree keeps its resources is not
    /// something to freeze into a machine's file.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub soundfont: Option<PathBuf>,

    /// The directory holding the user's own Roland ROMs, for
    /// `Music::Mt32`. There is no default and there never will be:
    /// nothing of Roland's is redistributable (doc 20 §4).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mt32_roms: Option<PathBuf>,

    /// Which of our own emulator fast paths this machine runs with
    /// (`Optimization`), holding only what someone turned off — absent
    /// means all of them at their shipped setting.
    ///
    /// **Last in the struct on purpose.** It is the only field that
    /// serializes to a TOML *table*, and a table swallows every
    /// key-value line that follows it: written anywhere else, the fields
    /// after it would be read back as part of `[optimizations]`.
    #[serde(default, skip_serializing_if = "Optimizations::is_empty")]
    pub optimizations: Optimizations,
}

/// How a family runs unless the machine says otherwise.
///
/// **Win98 is emulated by default.** KVM runs the guest at host speed,
/// and doc 06's `pentium3` model does not protect against Win9x's
/// fast-CPU bugs — it is the *speed* that trips them, not the CPUID. TCG
/// is also the path this project's own x87/SSE fast paths (docs 13, 16)
/// exist for, so it is the configuration Win98 is actually tuned and
/// tested on here. XP has none of those problems and wants the speed.
pub fn default_accel(family: Family) -> Accel {
    match family {
        // A DOS machine is throttled by default and a throttle needs TCG
        // (`-icount` and KVM cannot coexist), so this is the only honest
        // default; `Auto` would promise KVM and not deliver it.
        Family::Win98 | Family::Dos => Accel::Tcg,
        // Nothing here is tuned for an era Linux or BeOS, and neither
        // has Win9x's fast-CPU bugs: take the host's speed when it is
        // there, emulate when it isn't.
        Family::Xp | Family::Other => Accel::Auto,
    }
}

/// Networking is on unless a bundle says otherwise: this is what every
/// machine did before the field existed, and a machine must not lose its
/// card by being read by a newer launcher. It is *not* what a new
/// machine gets — that is `default_network`, and since 2026-09-07 it is
/// off for every family.
fn network_enabled_default() -> bool {
    true
}

/// Whether a *new* machine of this family gets a card. **None of them
/// do** (the user's decision, 2026-09-07): these guests stopped getting
/// security fixes twenty years ago, so a machine that is on a network
/// before anyone asked for it is the wrong way round — the checkbox is
/// right there in the wizard for the machine that wants one, and turning
/// it on later is a card appearing, which Windows handles far better
/// than one disappearing. DOS had never got one anyway, for a reason of
/// its own: it reaches a network only through a packet driver the user
/// installs by hand, so the card would be an unused device the guest
/// still enumerates.
///
/// The family is still the argument, because that is what a default here
/// is allowed to depend on and one of them may want a card again.
///
/// (An existing bundle with no `network` field is unaffected — that is
/// `network_enabled_default`, which stays *on* for every family, because
/// taking a card away from a machine that has been running with one is a
/// hardware change and not a default.)
pub fn default_network(_family: Family) -> bool {
    false
}

/// The tablet is on unless a bundle says otherwise: it is what every
/// machine had before the field existed, and desktop mousing without a
/// grab is the right first impression of a machine.
fn seamless_mouse_default() -> bool {
    true
}

/// Whether a *new* machine of this family gets the tablet. Two families
/// don't. DOS's mouse drivers talk to the PS/2 controller, so a tablet
/// would leave the guest with a pointer it cannot see. `Other` is off
/// for the weaker version of the same reason: an absolute USB pointer
/// needs the guest's USB HID stack *and* its windowing system to agree
/// it is absolute, which an era Linux (XFree86 wants an explicit
/// `evdev`/`usbtablet` input section) and BeOS do not do out of the box
/// — and unlike the Windows families there is no guest-tools install
/// that would fix it. The PS/2 mouse works everywhere, so that is what a
/// machine we cannot test starts with; the checkbox turns it on for a
/// guest that does handle it. (An existing bundle with no
/// `seamless_mouse` field is unaffected, for the same reason
/// `network_enabled_default` is unconditional.)
pub fn default_seamless_mouse(family: Family) -> bool {
    !matches!(family, Family::Dos | Family::Other) && seamless_mouse_default()
}

/// The speed a family runs at unless the machine says otherwise. Only
/// DOS is throttled: a 486DX2-66 is the machine most of the CD-ROM era
/// was written for, and it is inside the range where the cap is exact
/// (see `CpuSpeed`). Windows machines are unthrottled — 9x and XP read
/// the clock instead of counting instructions, and a throttle would only
/// make them slow.
pub fn default_cpu_speed(family: Family) -> CpuSpeed {
    match family {
        Family::Dos => CpuSpeed::Dx266,
        Family::Win98 | Family::Xp | Family::Other => CpuSpeed::Unthrottled,
    }
}

/// The adapter a family starts on, or `None` when it has no choice to
/// make. Always the first of `video_choices`, so the list and the
/// default cannot disagree: our own adapter on XP, where the whole
/// display path is built on it, Windows' in-box Cirrus on Win98, whose
/// driver of ours is newer than that, and the standard VGA on `Other`,
/// the one with a VESA path every guest can fall back on when it has no
/// native driver at all.
pub fn default_video(family: Family) -> Option<Video> {
    video_choices(family).first().copied()
}

/// doc 06's RAM default for a family.
pub fn default_ram_mb(family: Family) -> u32 {
    match family {
        Family::Win98 => 256, // doc 06: 256 MB default, ≤512 MB hard cap
        Family::Xp => 512,    // doc 06: 512 MB-1 GB default
        // DOS itself uses the first megabyte; the rest is XMS for the
        // extenders a mid-90s game ships with, and more of it buys
        // nothing. 64 MB is generous for the era and stays inside what
        // MS-DOS 6.22's own HIMEM.SYS manages.
        Family::Dos => 64,
        // No family default to inherit, so the number is the one that
        // suits the range of things this covers: an era Linux desktop or
        // BeOS R5 is comfortable in 512 MB and neither needs more.
        Family::Other => 512,
    }
}

/// A value inside a QEMU option string. Options are separated by commas
/// there, so a comma in a value is written twice — otherwise a disk in
/// `~/Games/Doom, Quake and friends/` silently becomes an unknown option
/// and QEMU refuses the whole line. Paths come from a file picker, and a
/// comma in a directory name is entirely ordinary.
fn opt_value(s: &str) -> String {
    s.replace(',', ",,")
}

/// What the UI lets a user ask for. The Win98 ceiling is doc 06's hard
/// cap, not a guess: Win9x fails to boot with much more than 512 MB (its
/// VCACHE sizing overflows), so offering 2 GB there would only produce a
/// machine that does not start. XP's is the practical 32-bit limit,
/// below the 3.5 GB where PCI space starts eating into RAM.
pub fn ram_mb_range(family: Family) -> std::ops::RangeInclusive<u32> {
    match family {
        Family::Win98 => 32..=512,
        Family::Xp => 64..=3072,
        Family::Dos => 4..=256,
        // The widest range we can honestly offer: we don't know what is
        // going in, so the only limits are the machine's. The bottom is
        // where a 1995 kernel still boots, the top is XP's 32-bit
        // ceiling. BeOS R5 is the one guest with a lower one of its own
        // (1 GB), which the wizard says rather than enforces.
        Family::Other => 16..=3072,
    }
}

impl Machine {
    /// A new machine from doc 06's reference defaults for `family`.
    pub fn reference(family: Family, name: String, disk: PathBuf) -> Self {
        Machine {
            name,
            family,
            ram_mb: default_ram_mb(family),
            accel: Some(default_accel(family)),
            network: default_network(family),
            seamless_mouse: default_seamless_mouse(family),
            disk,
            disc: None,
            discs: Vec::new(),
            shader_profile: None,
            shader: None,
            floppy: None,
            boot: None,
            cpu_speed: Some(default_cpu_speed(family)),
            video: default_video(family),
            sound: Some(default_sound(family)),
            music: Some(default_music(family)),
            soundfont: None,
            mt32_roms: None,
            pad: Some(default_pad(family)),
            optimizations: Optimizations::default(),
        }
    }

    /// The disc in the drive at boot: `disc`, or the first entry of a
    /// pre-shared-shelf bundle's `discs` (which is exactly what the old
    /// `qemu_args` attached).
    pub fn boot_disc(&self) -> Option<&PathBuf> {
        self.disc.as_ref().or_else(|| self.discs.first())
    }

    pub fn load(path: &Path) -> std::io::Result<Machine> {
        let text = std::fs::read_to_string(path)?;
        toml::from_str(&text).map_err(std::io::Error::other)
    }

    /// Writes the bundle in the current format, which also migrates a
    /// legacy one: the boot disc moves to `disc` and the old per-machine
    /// `discs` list is dropped. Its entries aren't lost — the library
    /// scan imports them onto the shared shelf
    /// (`DiscLibrary::import_legacy`) before anything here can rewrite a
    /// bundle.
    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        let mut out = self.clone();
        out.disc = self.boot_disc().cloned();
        out.discs.clear();
        let text = toml::to_string_pretty(&out).map_err(std::io::Error::other)?;
        std::fs::write(path, text)
    }

    /// The `qemu-system-i386` arguments the player expects on its own
    /// command line (`player -- <these>`), per doc 06's reference tables.
    /// `pc_bios_dir` is `qemu/pc-bios` (see README's `-L`); `shelf`, when
    /// given, is the flat disc-shelf file the drive answers the in-guest
    /// `CDSHELF` program from (`cdshelf/cdshelf_proto.h`).
    /// The accelerator, as `-accel` options. `Auto` is expressed as
    /// QEMU's own fallback list rather than by probing `/dev/kvm` here:
    /// the answer a probe gives can still be wrong at spawn time
    /// (permissions, a module unloaded since), and QEMU's list already
    /// means exactly "KVM if you can, emulation otherwise" — two
    /// `-accel` options are tried in order and the first that
    /// initializes wins, which is the same code path `accel=kvm:tcg`
    /// took. `kvm` is only offered where it exists at all — on macOS the
    /// name is not a registered accelerator, and listing it there would
    /// print a warning on every boot for nothing.
    ///
    /// **`-accel`, not `-machine accel=`, and the two cannot be mixed**
    /// ("The -accel and \"-machine accel=\" options are incompatible",
    /// `system/vl.c`). The TCG-side optimizations are properties of the
    /// accelerator object, and `-accel` is the only spelling that has
    /// somewhere to put them.
    fn accel_args(&self) -> Vec<String> {
        let mut tcg = "tcg".to_string();
        tcg.push_str(&self.optimization_props(Knob::Tcg));
        match self.effective_accel() {
            // "hardware acceleration, required": spelled the way the
            // host in hand spells it (`whpx` on Windows), so a machine
            // directory carries between them meaning the same thing.
            Accel::Kvm if cfg!(target_os = "windows") => vec!["-accel".into(), "whpx".into()],
            Accel::Kvm => vec!["-accel".into(), "kvm".into()],
            Accel::Auto if cfg!(target_os = "linux") => {
                vec!["-accel".into(), "kvm".into(), "-accel".into(), tcg]
            }
            Accel::Auto if cfg!(target_os = "windows") => {
                vec!["-accel".into(), "whpx".into(), "-accel".into(), tcg]
            }
            Accel::Auto | Accel::Tcg => vec!["-accel".into(), tcg],
        }
    }

    /// The `,name=on`/`,name=off` tail for one kind of switch — **only
    /// what differs from the property's own default in our QEMU**, so a
    /// machine that has changed nothing produces the command line it
    /// always produced, and one run against a QEMU without these patches
    /// still starts.
    fn optimization_props(&self, knob: Knob) -> String {
        let mut out = String::new();
        for opt in Optimization::ALL {
            if opt.knob() == knob && !self.optimizations.is_default(opt) {
                let value = if self.optimizations.enabled(opt) { "on" } else { "off" };
                out.push_str(&format!(",{}={value}", opt.key()));
            }
        }
        out
    }

    /// What this machine actually runs as: its own setting, or its
    /// family's (`default_accel`) when the bundle doesn't say —
    /// **except** that a throttled CPU forces emulation, because QEMU
    /// refuses `-icount` together with KVM ("cannot enable icount when
    /// KVM is enabled") and starting is better than being right about
    /// the accelerator. The wizard says so next to the field, so this
    /// never happens behind someone's back.
    pub fn effective_accel(&self) -> Accel {
        if self.effective_cpu_speed().icount_shift().is_some() {
            return Accel::Tcg;
        }
        self.accel.unwrap_or_else(|| default_accel(self.family))
    }

    /// The CPU speed this machine runs at: its own setting, or its
    /// family's default (only DOS has a throttled one).
    pub fn effective_cpu_speed(&self) -> CpuSpeed {
        self.cpu_speed.unwrap_or_else(|| default_cpu_speed(self.family))
    }

    /// The adapter this machine runs on, or `None` where the family
    /// settles it. A `video` naming an adapter this family does not
    /// offer falls back to its default rather than being obeyed:
    /// `video = "std"` on an XP machine would leave the guest with no
    /// driver at all (there is none for the Bochs adapter on XP), which
    /// is not something a stray bundle field should be able to do.
    pub fn effective_video(&self) -> Option<Video> {
        let choices = video_choices(self.family);
        let default = *choices.first()?;
        Some(match self.video {
            Some(v) if choices.contains(&v) => v,
            _ => default,
        })
    }

    /// What this machine does with a host gamepad: its own setting, or
    /// its family's default. A `pad` naming a setting this family does
    /// not offer falls back the way `effective_video` does. That is also
    /// where a bundle from a *later* launcher lands: `pad_lenient` has
    /// already turned its unknown `usb` or `gameport` into `None`, and
    /// this turns `None` into the family's default.
    pub fn effective_pad(&self) -> Pad {
        let choices = pad_choices(self.family);
        match self.pad {
            Some(p) if choices.contains(&p) => p,
            _ => default_pad(self.family),
        }
    }

    /// The `-vga` / `-device` pair that puts the machine's adapter on it.
    /// `-vga none` first for every choice, so the machine never gets the
    /// default adapter *as well as* the one it asked for.
    fn video_args(&self) -> Vec<String> {
        let mut args = vec!["-vga".to_string(), "none".to_string()];
        if let Some(video) = self.effective_video() {
            let [flag, value] = video.args();
            // `-vga <name>` replaces the `none` above rather than adding
            // to it; our own adapter is a `-device` and keeps it.
            if flag == "-vga" {
                args = vec![flag.to_string(), value.to_string()];
            } else {
                args.extend([flag.to_string(), value.to_string()]);
            }
        }
        args
    }

    /// The card this machine has. One the family does not offer falls
    /// back to its default rather than being obeyed, for the reason
    /// `effective_video` does the same: a stray field should not be
    /// able to produce a machine whose guest has no driver.
    pub fn effective_sound(&self) -> Sound {
        let choices = sound_choices(self.family);
        match self.sound {
            Some(s) if choices.contains(&s) => s,
            _ => default_sound(self.family),
        }
    }

    /// What is on this machine's MIDI port.
    pub fn effective_music(&self) -> Music {
        let choices = music_choices(self.family);
        match self.music {
            Some(m) if choices.contains(&m) => m,
            _ => default_music(self.family),
        }
    }

    /// The sound card and the MIDI port, as devices (doc 20 §6).
    ///
    /// The bank for a General MIDI port is named here **only when the
    /// user chose one**: the one we ship is a companion of the player's,
    /// found by the player's own rule the way the Glide wrapper is, so
    /// that a package that moves does not invalidate every machine file
    /// in the library.
    fn audio_args(&self) -> Vec<String> {
        let mut args = self.effective_sound().args();
        let music = self.effective_music();
        if music != Music::None {
            let mut spec = format!("mpu401,audiodev=embed0,synth={}", music.key());
            match music {
                Music::Gm => {
                    if let Some(sf2) = &self.soundfont {
                        spec.push_str(&format!(",soundfont={}", opt_value(&sf2.display().to_string())));
                    }
                }
                Music::Mt32 => {
                    if let Some(dir) = &self.mt32_roms {
                        spec.push_str(&format!(",romdir={}", opt_value(&dir.display().to_string())));
                    }
                }
                Music::None => {}
            }
            args.extend(["-device".to_string(), spec]);
        }
        args
    }

    pub fn effective_boot(&self) -> Boot {
        self.boot.unwrap_or_default()
    }

    pub fn qemu_args(&self, pc_bios_dir: &Path, shelf: Option<&Path>) -> Vec<String> {
        // Windows 98 has no driver for an HPET (`PNP0103` is in none of
        // 98 SE's INFs) and never uses one — it times off the PIT — so on
        // 98 it is an "Unknown Device" with a yellow mark in Device
        // Manager and nothing else. QEMU's fw_cfg (`QEMU0002`) is the one
        // other device 98 has no driver for, but its `_STA` says "not
        // shown in UI", and 98 obeys that.
        let machine = if matches!(self.family, Family::Win98) { "pc,hpet=off" } else { "pc" };
        let mut args = vec!["-L".into(), pc_bios_dir.display().to_string(), "-machine".into(), machine.into()];
        args.extend(self.accel_args());
        args.extend([
            "-m".into(),
            self.ram_mb.to_string(),
            // doc 06's floor for both families: avoids the fast-CPU Win9x
            // bugs and CPUID-dispatched guest code that mis-decodes under
            // -cpu host (the Max Payne JPEG decoder gotcha)
            "-cpu".into(),
            format!("pentium3{}", self.optimization_props(Knob::Cpu)),
            "-drive".into(),
            format!("file={},if=ide,index=0,media=disk", opt_value(&self.disk.display().to_string())),
        ]);
        // The pointer (doc 03). The USB tablet is an absolute device: it
        // reports where the pointer is rather than how far it moved, so
        // the host pointer *is* the guest cursor and nothing has to be
        // grabbed. Without it the machine keeps the PS/2 mouse alone —
        // relative, grabbed on a click — and the controller goes with
        // the tablet, because the tablet is the only thing on it.
        // ...and the gamepad (M13 path A), which needs the same
        // controller. `-usb` goes on once for both: QEMU takes a second
        // one, but it is the kind of line nobody reads twice, and a
        // machine whose pointer is turned off must still get a
        // controller for its pad rather than a `-device usb-gamepad`
        // with no bus to attach to.
        let pad_usb = self.effective_pad() == Pad::Usb;
        if self.seamless_mouse || pad_usb {
            args.push("-usb".into());
        }
        if self.seamless_mouse {
            args.extend(["-device".into(), "usb-tablet".into()]);
        }
        if pad_usb {
            args.extend(["-device".into(), "usb-gamepad".into()]);
        }
        // ...and path B's gameport (patch 27), which needs no controller
        // of any kind: it is an ISA device at 0x200-0x207, and every
        // machine this project makes has an ISA bus. It is also the only
        // pad device DOS can use, which is why that family is offered it
        // and not the USB one.
        if self.effective_pad() == Pad::Gameport {
            args.extend(["-device".into(), "gameport".into()]);
        }
        // The CPU rate, when the machine asks for one. `align=on` is the
        // whole point and not a detail: `-icount shift=N` on its own only
        // makes the *guest's* clock a function of instructions retired,
        // which leaves the guest believing it is slow while the host runs
        // it as fast as it likes — measured 2026-09-06, a run that was
        // meant to be throttled finished in less wall-clock time than the
        // unthrottled one, because the guest's idle waits collapse too.
        if let Some(shift) = self.effective_cpu_speed().icount_shift() {
            args.extend(["-icount".into(), format!("shift={shift},align=on")]);
        }
        // The floppy: `format=raw` because a floppy image has no header
        // to probe and QEMU otherwise both warns and refuses writes to
        // the boot sector.
        if let Some(floppy) = &self.floppy {
            args.extend(["-drive".into(), format!("file={},if=floppy,index=0,format=raw", opt_value(&floppy.display().to_string()))]);
        }
        if let Some(order) = self.effective_boot().order() {
            args.extend(["-boot".into(), format!("order={order}")]);
        }
        // Doc 06's per-family NIC on QEMU's user-mode NAT, or no adapter
        // at all — not an unplugged cable: a card that is present would
        // still make Windows enumerate it, ask for its driver on a fresh
        // install and wait on it at boot, none of which is what turning
        // networking off is for.
        //
        // `-nic none` is the half that actually turns it off. QEMU
        // *creates a NIC of its own* when the command line asks for no
        // networking at all — leaving out the `-netdev` doesn't remove
        // the card, it only replaces ours with an e1000 in the slot
        // below (`query-pci` says so), which is the opposite of what the
        // setting means.
        //
        // The Windows devices carry explicit PCI addresses because
        // removing the NIC would otherwise slide the card below it up
        // into its slot, and a card that moves is a hardware change an
        // installed Windows re-detects. These are the addresses those
        // devices already get from their `-device` order today, so
        // pinning them changes nothing for an existing machine — it only
        // keeps them still when the NIC comes and goes. DOS needs none of
        // it: its display is `-vga` (not a `-device`) and its SB16 is
        // ISA, so its NIC is the only card in the sequence.
        if !self.network {
            args.extend(["-nic".into(), "none".into()]);
        }
        match self.family {
            // The adapter is a *choice* here since 2026-09-07
            // (`video_choices`): `d3dpt-vga` with our own display driver
            // (doc 19, M10) — the linear frame buffer the player scans
            // out, the mode table, the page flips that pace a game,
            // Direct3D through the driver — or the Cirrus and the in-box
            // driver Windows already has, which has none of that and is
            // where this family **starts**: the 9x driver is much newer
            // than XP's, so a new 98 machine comes up on Windows' own and
            // is moved to ours deliberately.
            //
            // **Changing it is a hardware change to an installed guest.**
            // That is a real consequence and not a detail: the guest
            // finds an unknown adapter, comes up in plain VGA, and wants
            // a driver — ours from the guest-tools ISO (`SETUP`, doc 19
            // §16), or Windows' own for the Cirrus — before it has its
            // desktop back. The machine boots either way.
            Family::Win98 => {
                args.extend(self.video_args());
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    // in-box 98 driver
                    args.extend(["-device".into(), "pcnet,netdev=n0,addr=0x03".into()]);
                }
                args.extend(self.audio_args());
            }
            // The 1994 PC: the same chipset and the SB16 doc 06 already
            // puts on the Win98 machine "for DOS boxes/games", one of the
            // two standard adapters for its VGA and VESA modes, and
            // nothing else. No 3D of any kind is reachable from DOS here
            // — the Glide wrapper for DOS is GLIDE2X.OVL, which we do not
            // build.
            //
            // The adapter is a *choice* here too since 2026-09-09
            // (`video_choices`), and the only family where it is not a
            // driver question: a DOS title programs the registers itself,
            // so what a different adapter changes is which VESA BIOS it
            // finds. It starts on the standard VGA — the fuller of the
            // two — where the hardcoded line it replaces said `cirrus`.
            Family::Dos => {
                args.extend(self.video_args());
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    args.extend(["-device".into(), "pcnet,netdev=n0".into()]);
                }
                args.extend(self.audio_args());
            }
            Family::Xp => {
                args.extend(self.video_args());
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    // in-box XP driver
                    args.extend(["-device".into(), "rtl8139,netdev=n0,addr=0x03".into()]);
                }
                args.extend(self.audio_args());
            }
            // Everything else of the era: standard hardware only, chosen
            // for having had a driver in the box on BeOS R5 and on a
            // period Linux alike, because there is no guest-tools install
            // to add one afterwards.
            //
            // Its two adapters are the two *standard* ones — never
            // `d3dpt-vga`, which needs our display driver and so exists
            // for Windows only — so this family has no 3D path of any
            // kind whichever is picked.
            //
            // The PCI addresses are pinned for the same reason the
            // Windows families pin theirs — removing the NIC would slide
            // the sound card up into its slot, and a card that moves is a
            // hardware change a guest re-detects. Every adapter here
            // takes 0x02 (measured), so these two follow it.
            Family::Other => {
                args.extend(self.video_args());
                if self.network {
                    args.extend(["-netdev".into(), "user,id=n0".into()]);
                    // in-box on BeOS R5 and on Linux since 2.2 (8139too)
                    args.extend(["-device".into(), "rtl8139,netdev=n0,addr=0x03".into()]);
                }
                // The Ensoniq AudioPCI, not the AC'97 the Windows
                // families get: it is the PCI card of the period both
                // these guests drive in the box (BeOS ships an ensoniq
                // add-on, Linux `snd-ens1370`), where AC'97 needs a
                // driver an era install may not have.
                args.extend(self.audio_args());
            }
        }
        // The CD-ROM drive is always attached, empty tray and all: a
        // real machine of the era has one, and the launcher's live disc
        // swap (`control.rs`) needs a device to put a disc *into* — a
        // drive that only exists when the bundle happened to ship a disc
        // couldn't be loaded later. The id is what a medium change
        // addresses (`control::CDROM_ID`).
        let mut drive = "if=none,id=cd0,media=cdrom".to_string();
        if let Some(disc) = self.boot_disc() {
            // `qemu_medium`, not the path: a shared folder is a disc too,
            // and it is named to QEMU with the `isodir:` prefix (M5g).
            drive.push_str(&format!(",file={}", opt_value(&crate::disc_library::qemu_medium(disc))));
        }
        let mut cd = format!("ide-cd,bus=ide.1,id={},drive=cd0,audiodev=embed0", crate::control::CDROM_ID);
        if let Some(shelf) = shelf {
            // The drive answers the in-guest CDSHELF program from this
            // file (patch 52). Without it the vendor command reports
            // "no shelf" and the drive is an ordinary CD-ROM — which is
            // also what a hand-written bundle run straight through
            // `player` gets.
            cd.push_str(&format!(",shelf={}", opt_value(&shelf.display().to_string())));
        }
        args.extend(["-drive".into(), drive, "-device".into(), cd]);
        args
    }
}
