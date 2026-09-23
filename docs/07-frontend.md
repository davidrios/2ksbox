# 7. Front end: player + launcher

There are two programs (ADR-005). The **player** runs one machine in one
window, and the **launcher** manages the library and spawns a player per
machine. This document covers both, the split between the launcher's
core and its Qt front end, the C ABI, and the package layout. The
display pipeline and input model are doc 03, the embed API doc 11, the
machine definitions per family doc 06, and the build and packaging
commands `docs/development.md` and `docs/build-macos.md` /
`build-windows.md`. Every check named here is in `docs/testing.md`.

## Player

- **What it runs is a QEMU command line**, not a bundle:
  `player [--shader <preset>] [--shader-params k=v,…] [--pad <mode>]
  -- <qemu args>`. The launcher translates a bundle into that line
  (`launcherx --print-player-args <bundle>`), so everything a bundle
  means lives in `launcher-core`. The player boots QEMU in-process (doc
  11) and renders through doc 03's pipeline. It runs one machine per
  process, because QEMU's cleanup is incomplete upstream.
- **Window.** The shaded display fills it, aspect-correct with black
  bars. Ctrl+Alt+Shift+F is borderless full screen.
- **Input** is doc 03's model: the absolute USB tablet for desktop
  mousing, a relative PS/2 grab for games (Ctrl+Alt+G releases), the
  host's shortcuts going to the guest while the window has focus
  (Ctrl+Alt+K toggles), Ctrl+Alt+Shift+D for Ctrl+Alt+Del, and
  Ctrl+Alt+S for a shot of the guest's own frame. A close with Alt held
  (Alt+F4) asks first, in a panel the player draws itself over the
  finished picture (`player/src/prompt.rs`). The player has no toolkit,
  and Linux has no message box that also works inside the Flatpak and
  over a full-screen window. Gamepads are M13
  (`docs/tracks/m13-gamepads.md`) and sit beside the grab model: a pad
  works whether or not the pointer is grabbed and never changes the
  grab.
- **Audio.** QEMU's mixer writes f32 into a lock-free ring drained by
  cpal (CoreAudio / WASAPI / PipeWire), and the player limits the sum
  rather than letting it clip. Pacing and the ring are doc 11 ("The
  audio driver and its pacing"). CD-DA mixes QEMU-side (doc 17 §5.4).
- **Not built: an overlay** for pause, snapshot, disc swap and the
  shader preset. Snapshots and disc swaps are the launcher's (live, over
  QMP, below), and a disc swap from inside the guest is `CDSHELF`.

## Launcher

The launcher is optional by design. `launcherx --play <bundle>` or a
hand-typed player line runs a machine with nothing else.

### The library

- A grid of machines shows the family and running state, and Play
  spawns a player. "Running" is the launcher's own child *or* a
  listening monitor socket, so a player started by `--play` counts too.
  The launcher only observes (`try_wait`), and a spawned player outlives
  it.
- **The launcher has no Stop or Kill**, on purpose. A killed guest
  leaves a dirty FAT, so a run ends from the guest or the player window.
- **Every Play is logged with the line it ran** (player, shader
  arguments, `--` and every QEMU argument, quoted to paste back into a
  shell) as `[player] …` in `launcher.log`, at the head of `player.log`,
  and on the terminal when there is one. The line is derived at spawn
  (family devices, shelf, QMP socket, shader profile), so a bundle alone
  does not say what ran.
- **The player is found** at `LAUNCHER_PLAYER_BIN`, else the installed
  prefix's `2ksbox-player`, else beside the launcher, else the root
  workspace's `target/<same profile>/player`. The last fallback exists
  because `launcher-qt` is its own workspace and builds into
  `launcher-qt/target/`, where no player sits. `--paths` prints the
  player it will use, and the grid's elided status label carries the
  whole sentence as a tooltip.
- **Clone…** (`launcher-core/src/clone_machine.rs`) makes a new machine
  under a name the user picks (offered as "<name> (copy)", numbered when
  taken) with the same settings and **its own copy of the disk**,
  wherever that disk is. "Use an existing disk" can point anywhere, and
  two machines on one image corrupt it the day both run. Internal
  snapshots come along inside the qcow2. Files the bundle names inside
  its folder are copied and renamed. What it names outside (shelf discs,
  a shader, a SoundFont) stays shared. A relative backing file is made
  absolute in the copy (`qemu-img rebase -u`). A running machine is
  refused. The copy runs on a thread with a progress bar and writes
  `machine.toml` last, so a clone in progress (or failed, whose folder
  is removed) never shows in the grid. It cannot be cancelled once
  copying, because `std::fs::copy` keeps the kernel's fast paths
  (reflinks, `copy_file_range`) and cannot stop mid-file. `launcherx
  --clone <machine.toml> [name]` and `lc_machines_clone` are the same
  model.
- **Not built:** last-frame thumbnails in the grid, and bundle
  import/export.

### The bundle

A machine is a directory with `machine.toml` (`launcher-core/src/
bundle.rs`), usually its disk, and references to shelf discs. These
rules keep it portable and readable:

- **Most settings are optional fields.** An absent `accel`, `video`,
  `sound`, `music` or `pad` follows the family. An absent field that
  predates the family rules means what every bundle before it ran
  (`seamless_mouse` on, `voodoo2` off, `network` off).
  `[optimizations]` stores only the switches that differ from the
  shipped setting, keyed by QEMU property name, so a new switch arrives
  at its default in every existing bundle, and an entry a newer launcher
  wrote survives a load and save in an older one.
- **`[optimizations]` must stay the last field of `Machine`.** It is the
  one field that serialises as a TOML table, and a table swallows every
  key after it.
- **Editing never renames the bundle directory**, even when the name
  changes, so outside references and the disk inside stay valid. New
  directories are slugs of the name, deduplicated (`xp-test-box`,
  `xp-test-box-2`).
- A bundle is validated before it is written. A corrupt `machine.toml`
  in the library is skipped with a `[library]` log line, never fatal.
- **Commas in paths are doubled** in every QEMU option string the
  bundle writes (disk, floppy, disc, shelf), since QEMU splits options
  on commas. Before this, a disk in `~/Games/Doom, Quake and friends/`
  made the whole line an unknown option. The `dirshelf` check hands QEMU
  such a path. A live insert passes a JSON string and needs no doubling.
- Legacy per-machine shelves (`discs = [...]`) are still read.
  `DiscLibrary::import_legacy` folds them onto the shared shelf at
  start-up (deduplicated by path, so it can run every time),
  `boot_disc()` falls back to the first entry, and `save` drops the
  field.
- The acceleration is written host-neutrally (below), so a directory
  copied between hosts keeps its meaning.

### The settings form

The form is one window with a sidebar of sections and a page each, laid
out the way VirtualBox and UTM lay theirs out (user request): General
(family, name), System (memory, processor, acceleration, emulation
optimizations, extra QEMU arguments), Display (adapter, Direct3D, the
Voodoo 2, the shader profile), Audio (sound card, music, SoundFont,
MT-32 ROMs), Input (gamepad, pointer), Network, Storage (disk, install
media, floppy, boot order). The sections and their order are the
model's (`wizard::Section`, `lc_wizard_label(LC_LABEL_SECTION, …)`).
Which field sits on which page is the front end's. The form opens on
its first page for a new machine or a different one, and where it was
left when the same one is reopened.

The same form creates and edits. It never shows a QEMU command line,
and it has no raw-TOML box (user decision). The fields are the way to
edit a machine, and a hand edit is the file itself.

**Every per-family field follows the family until someone picks it.**
Memory, the accelerator, the processor, the NIC and the pointer do, and
so do the four fields whose *list* is per family: adapter, sound card,
music port and pad. Each has a `*_chosen` flag in `wizard::Form`.
`choose_family` moves every unchosen field to the new family's default.
`reset_*` clears the flag, so "Default" follows the family again rather
than pinning the value. `open_edit` sets every flag, because an
existing machine's values are deliberate. A field with a consequence
has no setter, only `choose_*`. The `capi` smoke asserts both
directions and "Default" (the C ABI has no pad row yet, so the pad is
not asserted there). The disk size follows the same rule
(`bundle::default_disk_size_gb`: 10 GB Win98 and Other, 20 GB XP, 2 GB
DOS; user decision).

The fields, and why each is what it is:

- **Family.** Win98, XP, DOS and Other (doc 06). **Other is the one
  family with a sentence under the picker** (`family_note()`). It is
  defined by what it does not get (our adapter, the whole 3D
  pass-through, all Windows components) and by hardware chosen for
  guests nothing here tests (BeOS, a period Linux, OS/2). The note
  names what it has (a VESA VGA, an RTL8139, an ES1370) and says there
  is no 3D.
- **Memory** is bounded per family (`bundle::ram_mb_range`: Win98
  32–512, since more will not boot; XP 64–3072, DOS 4–256, Other
  16–3072). BeOS R5's 1 GB ceiling is stated, not enforced.
- **Acceleration** is Automatic / hardware-required / Emulation,
  `accel = "auto" | "kvm" | "tcg"` on every host. `kvm` means "hardware
  acceleration, required" and is spelled per host at spawn (`whpx` on
  Windows, and the label reads "KVM (required)" / "WHPX (required)").
  Required really refuses to start without it. *Automatic* is QEMU's own
  fallback list (`-accel kvm -accel tcg`, `whpx` then `tcg` on Windows),
  not a probe of ours whose answer could be stale by spawn time. macOS
  has no hardware accelerator for an x86 guest, so there Automatic is
  TCG. `player::hw_accel_available()` backs only the hint beside the
  picker. Linux opens `/dev/kvm` for *writing* (a bare `exists()` misses
  a user outside the `kvm` group). Windows asks `WHvGetCapability`,
  since the feature can be installed and still off or held by
  Hyper-V/WSL2. **Win98 and DOS default to emulation, XP and Other to
  Automatic.** KVM runs Win9x at host speed, and `-cpu pentium3` does
  not protect it from its own fast-CPU bugs. Emulation is also what the
  fast paths of docs 13 and 16 exist for, so it is what Win98 is tuned
  and tested on. DOS is throttled by default, and a throttle needs TCG.
  A bundle with no `accel` follows its family.
- **The processor** is a combo of named machines (`cpu_speed`,
  `bundle::CpuSpeed`): *Unthrottled* down through *Pentium 133*,
  *486DX2-66*, *386DX-33*, *286-12*. "It needs a 486" is on the box,
  and DOS-era software times itself against the CPU it finds, so this
  field decides whether a game is playable (doc 06 has the
  measurements). It is offered on every family (a Win98 DOS box runs DOS
  games too), and only DOS defaults to a throttled one (486DX2-66). A
  throttle is `-icount`, which cannot run under KVM, so
  `effective_accel()` returns TCG whenever one is chosen, and the form
  says so.
- **Emulation optimizations** have one checkbox per switch of our QEMU
  patch queue (`bundle::Optimization::ALL`, `patches/qemu/README.md`):
  `x87-fast`, `sse-fast`, `simd-fast`, `rep-fast`, `x87-pc64-as-53` on
  `-cpu`; `smc-same-value`, `soft-imm`, `inline-lookup`,
  `tb-invalidate-fast`, `tlb-floor`, `tls-hot-paths`, `jump-cache-keep`,
  `eob-chain`, `tlb-retire` on `-accel tcg`. Every patch has had a
  switch since patch 29 (before it, "all off" left patches 15, 16 and 19
  in). **They are exposed because the switch is the oracle.** Each
  replaces simulated arithmetic with the host's, so one run with one box
  clear says whether a fast path made a guest compute the wrong number,
  with no bisecting of a patch queue against a Windows install. All ship
  on except `x87-pc64-as-53`, the one that changes what the guest
  computes. The section is a disclosure headed "Emulation optimizations
  (N of M on)", so a machine with one off says so while closed. It
  gives each switch's measured gain, and says they do nothing on a
  machine headed for KVM. "All defaults", "Turn all off" and "Turn all
  on" sit above them (`Optimizations::disable_all` / `enable_all`,
  `Form::*_all_optimizations`). "All on" is not the defaults because of
  `x87-pc64-as-53`, and the note says which of the three states the
  machine is in. Only the difference is stored, keyed by the QEMU
  property name. Patch 21's `pinned-regs` is not offered (user
  decision: it crashed guests for too small a gain). A bundle still
  carrying it keeps the entry but never emits it, until "All defaults"
  removes it (`Optimizations::RETIRED`). The accelerator is spelled
  `-accel kvm -accel tcg,…` rather than `-machine accel=kvm:tcg`,
  because the accelerator properties need somewhere to live and QEMU
  refuses the two spellings together. Two `-accel` options are tried in
  order, exactly as `kvm:tcg` was.
- **Extra QEMU arguments** (user request) is one line, the escape hatch
  for what the form has no field for (`-global d3dpt-vga.ddflags=32768`,
  a trace). Whitespace separates, single or double quotes group and are
  removed, and there is no escape character, so a Windows path's
  backslashes stay as typed (`bundle::split_args` / `join_args`). It is
  stored as the list `extra_qemu_args` and appended **last**, so a
  repeated option is the user's. Nothing validates the arguments. An
  unclosed quote is an orange note while typing and a refusal at save.
- **The display adapter** is the one picker whose *list* changes with
  the family (`bundle::video_choices`, doc 06). Windows chooses between
  our adapter with our driver and the Cirrus with its in-box driver,
  Other between the two standard adapters, and DOS between `std` and
  `cirrus` (which VESA BIOS a title finds). A new machine starts on the
  list's first entry, which is ours on both Windows families. An adapter
  a family does not offer is refused rather than stored (`std` on XP
  would leave the guest no driver). Changing an existing machine's
  adapter draws an orange line, because the guest will find new
  hardware on its next start.
- **Direct3D** picks which Direct3D 9 the executor runs on
  (`bundle::D3d9`, ADR-007's second amendment). It shows only on a
  machine with our adapter, which carries the executor
  (`d3d9_applies()`), and offers only what this host can run
  (`bundle::d3d9_choices`): Automatic and DXVK everywhere, plus "This
  PC's own Direct3D 9" on Windows. On Windows the launcher's Vulkan
  probe resolves Automatic (`d3d9=system` when there is no Vulkan 1.3
  GPU or only a software one). Elsewhere Automatic writes nothing, and
  the device's own `exec=auto` takes Wine on the host when DXVK finds no
  device (ADR-018). Unlike the rest of the 3D story this is a picker,
  because a host can have both and the user sees whether a game draws
  right. Automatic is for everyone, and the others are an A/B. A bundle
  saying `system` opened on another host shows Automatic and keeps its
  value. A machine moved to another adapter keeps the setting too, since
  the command line simply stops saying it.
- **What this host gives the guest's Direct3D is stated, not chosen**
  (ADR-013), in the note under that picker (`d3d9_note()`;
  `graphics_note()` keeps the same sentence for a front end with no
  picker, the C smoke among them). The host settles it, and the only
  failure worth preventing is finding out after the machine exists.
  `launcher-core/src/host_gpu.rs` (`HostGpu::backend`, `d3d_headline`,
  `d3d_advice`) gives one of these answers:
  - a Vulkan 1.3 GPU: DXVK;
  - a **software** Vulkan driver: available, drawn orange. It works and
    disappoints, so the note says to expect it very slow and that the
    other stack may beat it. With a Wine present too it suggests
    `-global d3dpt-vga.exec=wine` in the extra arguments, since which
    of two working stacks is faster is the box's to answer;
  - Windows below the floor: "runs on this PC's own Direct3D 9";
  - Linux or macOS below the floor with a Wine (found by the remote
    library's rule: `D3DPT_WINE`, the Mac apps, `PATH`) and the
    executor's Windows build shipped (`lib/2ksbox/wine/
    d3dpt-exec-host.exe`): "runs through Wine on this host", slower than
    a Vulkan GPU;
  - neither: a plain note naming the Wine to install, and WineD3D in the
    guest until M15's last step.

  While our adapter is picked on a host with no executor, the note adds
  "Keep the 2ksbox adapter anyway. Only its Direct3D needs Vulkan.".
  The driver offers Direct3D only after `D3D_STATUS` says the executor
  loaded, and the Cirrus has no Direct3D either, while losing the flip
  chain's vertical blank, the 8 bpp modes, gamma and the cursor. The
  adapter is never picked from the host, because an image moves between
  hosts and changing its adapter is a driver install. DOS and Other get
  no note, since the guest half of the pass-through is Windows DLLs.
  `launcherx --host-check` is the full answer for a script or support
  question (exit 0 through Wine). `--paths` prints the Wine and the pair
  as `wine` and `wine-host`, and the Vulkan the probe ran on as `vulkan`
  and `vulkan-icd`. The probe runs on the package's own Vulkan where the
  package carries one, and on the system's otherwise. In the macOS app
  that is `host_gpu::shipped_loader` opened by full path, with the app's
  ICD named by `host_gpu::announce_driver`, every front end's first
  call in `main` (`lc_announce_driver` in the C API). Asking dyld for
  `libvulkan.dylib` by leaf name instead made the packaged launcher on a
  Mac without Homebrew answer "not present" while the player ran DXVK.
- **The Voodoo 2** ("Emulated 3dfx Voodoo 2", `voodoo2`; doc 21) sits
  under the adapter, as the card sat beside a 2D card. When on, the
  machine gets `-device voodoo2,addr=0x05` and nothing else changes. It
  is off unless picked, on every family, because a card the guest has
  no driver for is a New Hardware wizard on every boot. **"Undo its
  dither"** (`voodoo2_undither`, doc 21 §12) is on the same line because
  it is a setting of that card only. It is disabled without the card
  (`voodoo2_undither_enabled()`), turned off with it, and written only
  where there is a device to carry it. It costs ~1.4 ms of the main
  loop per presented frame, hence a choice. The notes are
  `voodoo2_notes()`.
- **Sound card and music** are per-family lists (`bundle::Sound`,
  `bundle::Music`, doc 20 §6). The FM chip comes with the card that
  carried one.
- **Seamless mouse** (`seamless_mouse`) is on for Win98 and XP and off
  for DOS (its mouse drivers read the PS/2 controller) and Other (an
  absolute pointer needs a guest USB stack nobody here vouches for). On
  gives `-usb -device usb-tablet`. The host pointer *is* the guest
  cursor and the window never grabs. Off leaves the chipset's PS/2
  mouse, grabbed on a click, which gives the relative movement mouselook
  needs. A game whose view sticks instead of turning needs this checkbox
  off; it is not a bug. The controller goes with the device. An absent
  field means on, what every bundle before it ran.
- **The gamepad** is `bundle::Pad` (M13).
- **Networking** is one checkbox, **off for every new machine** (user
  decision). These are unpatched systems, and ticking it later is a card
  *appearing*, which Windows handles far better than one disappearing.
  An absent `network` field means off too (user decision). On gives doc
  06's per-family NIC on user-mode NAT (outbound only). Off emits
  `-nic none`, because QEMU otherwise adds a NIC of its own. XP's PCI
  devices carry explicit addresses, so the NIC's absence does not slide
  the sound card into its slot and make an installed guest re-detect
  hardware.
- **A floppy and a boot order** (`floppy`, `boot`). *Boot from* is
  Automatic / Hard disk / Floppy / CD. Automatic emits no `-boot`, which
  is what the "boot the installer from the CD because the new disk is
  blank" case relies on.

### The disc shelf

- **One shelf serves every machine** (`discs.toml` beside the machine
  and profile libraries), because a rip belongs to the person, not the
  machine that installed it first. A machine keeps only which disc is in
  its drive at boot.
- **Sorted by label**, case-insensitively, with **digit runs compared as
  numbers** (`disc 10` after `disc 2`). It is an invariant of
  `DiscLibrary`, not a sort each view does, because the flat file the
  in-guest `CDSHELF` lists is addressed by slot number. A view that
  sorted for itself would offer one disc and load another. So a row
  index is good only until the next edit, and a rename in progress must
  not re-sort (Qt re-sorts on `editingFinished`). The `shelforder`
  check covers it.
- **"Browse…" adds the disc** the moment the dialog closes (user
  report). A picker whose only visible effect is a path in a box reads
  as one that did nothing. The field stays for a *typed* path.
  `PathField` emits `picked` beside `edited`. The `qt-shelf` check hands
  the field the path a dialog would have (a real dialog cannot be opened
  offscreen) and asks the window whether the shelf grew.
- **A host folder is a disc too** ("Add folder…"). The drive is given
  `isodir:<path>`, an ISO 9660 + Joliet volume generated over the tree
  (doc 17 §8, M5g). It is how a patch, a save game or a folder of
  installers reaches a guest without networking or mastering an image.
  It is read-only, a snapshot of the tree as the tray closed. Up to an
  80-minute CD it is a CD, above that the drive reports a DVD-ROM, and
  past a DVD-9 (8.1 GiB) `isodir` refuses the tree, which the row's
  error line shows with both sizes. A folder's label is its whole name
  (`patch1.3` keeps its `.3`).
- **`disc_library::qemu_medium(path)` is the one place a medium is named
  to QEMU**: `isodir:<path>` for a directory, the path for a file. It
  serves the boot drive, a live insert and the flat shelf file alike,
  and decides from the path each time, so a deleted folder is just a
  missing file.
- **The CD-ROM drive is always attached**, empty tray and all, with the
  fixed id `ide1-cd0` (`control::CDROM_ID`). A drive that existed only
  when the bundle had a disc could never be loaded later.
- **Insert and Eject force the tray.** QMP's `blockdev-change-medium`
  and `eject` otherwise *ask* a guest that has locked the medium (XP
  locks it for every open handle), and the click appears to do nothing,
  then takes effect minutes later. The user's click on this machine's
  own shelf is the whole authority `force` needs. Insert passes no
  `format`, so a `.cue`/`.ccd` still probes to the `cdimage` driver.
- **The one-click guest-tools disc** is the newest
  `guest-tools/out/guest-tools-*.iso` in a checkout or
  `share/2ksbox/guest-tools/` installed (`LAUNCHER_GUEST_TOOLS_ISO`
  overrides), canonicalised because it is written into a bundle.
- **The shelf from inside the guest** (`CDSHELF`, guest-tools ISO; the
  tool is in `docs/testing.md`) answers a disc-2 prompt without leaving
  the game. It is a window on Windows, a key-per-disc menu in DOS, and
  verbs for scripts. In the window Insert is grey while a disc is in the
  drive (user decision), so the tray is emptied with Eject as a step of
  its own. The verbs swap in one step. The channel is a vendor ATAPI
  command on the machine's own drive (opcode 0xD0, patch 52,
  `cdshelf/cdshelf_proto.h`), the one thing DOS, Win98 and XP can all
  send a raw command to (PIO, ASPI, SPTI). It needs no device and no
  driver, and a machine with no shelf answers ILLEGAL REQUEST. The
  launcher publishes the shelf to a flat file beside the monitor socket
  at spawn and on every edit. `launcherx --discs publish <bundle dir>`
  does the same by hand.

### How the launcher reaches a running machine

Snapshots and disc swaps on a running machine need its monitor, and the
player's own is a socketpair inside its process (doc 11). Rather than
give either binary an IPC interface, **the launcher adds `-qmp
unix:<runtime dir>/…,server,nowait` to the player's arguments and speaks
QMP to it itself**. QEMU allows several monitors, and everything after
`--` reaches QEMU unchanged. The socket is derived from the bundle
directory, in an owner-only directory (a monitor is complete control of
the machine). A stale one left by a killed player is removed before
spawn, since QEMU will not bind over it. **Windows uses the same
Unix-domain socket.** QEMU's Windows build binds `unix:` addresses and
the launcher's client is Winsock AF_UNIX (`control.rs`). A loopback
port would let any local process reach it, and a named pipe's chardev
waits for its one client inside machine start-up. A host that cannot
bind one (no AF_UNIX, a temp directory that cannot hold a socket, Wine)
is found by a trial bind and runs without live control. So does a path
longer than `sun_path`.

### Snapshots

- **Live** snapshots are QMP jobs (`snapshot-save` / `-load` /
  `-delete`), polled through `query-jobs` so the window never blocks
  while QEMU writes a guest's RAM. Buttons grey while one runs. A
  restore resumes the VM **only if it was running**. The snapshot node
  is looked up at run time (QEMU names it like `#block136`) and must be
  the one with `drv == "qcow2"`. A qcow2 shows as two nodes with the
  same filename, and only the format node holds snapshots.
- **Offline**, the launcher works on the qcow2 with `qemu-img snapshot`
  (the same snapshots `savevm` writes) and lists them with `qemu-img
  info --output=json`, a stable interface, where the table form cannot
  escape a tag with a space. `qemu-img`'s stderr is the window's error
  text.
- Each mode is refused in the other, because `qemu-img` writing an
  image QEMU has open corrupts it. Restore asks for confirmation (it has
  no undo, and it sits beside Delete).

### Shader profiles, presets and the preview

- **A profile** is a name, a `.slangp` path and a **sparse** override
  table. Only parameters someone moved are stored, so a profile follows
  the preset's own retuning and survives new parameters. A machine's
  `shader_profile` wins over its raw `shader` path (the hand-edit escape
  hatch). Both become the player's `--shader` / `--shader-params` at
  spawn, and the player skips an unknown parameter with a log line.
- **The form's picker has the same shape as every other picker.**
  `Form::shader_profile_labels` gives its rows: the app default
  (`SHADER_DEFAULT_LABEL`, the grid's word too) and then the library by
  name. `shader_profile_index` is the row a machine is on (0 for a
  profile that no longer exists, which is what the machine plays with),
  `choose_shader_profile` / `reset_shader_profile` are the verbs, and
  the "Default" button sits beside it. The Qt window rescans the
  library when a profile is saved or deleted while the form is open
  (`refreshProfiles`).
- **Where presets come from** (`shader_source::presets_dir()`):
  `LAUNCHER_SHADERS_DIR` if set (then nothing else), else the checkout's
  `third_party/slang-shaders`, else a downloaded copy in the data
  directory. "Has presets" means a `.slangp` within two levels, so an
  empty or half-unpacked directory reads as none.
- **The download** is upstream's `master` tarball (a package has no pin
  to read, and overrides are by name, so a newer tree is additive) over
  HTTPS with `ureq`/rustls (no system OpenSSL), streamed through
  `flate2` + `tar` on its own thread, showing MB so far (codeload sends
  no `Content-Length`). It unpacks into a `.part` sibling and renames
  over the old collection only once the result has presets, so an
  interrupted download never reads as installed. Symlinks, special
  entries and `..` paths are skipped. It is offered as a button in the
  profile manager, and **once at start-up** (`launcher_core::firstrun`)
  when no collection exists anywhere, as a modal question over the grid,
  because a button two windows deep is where a new user never looks.
  Answering either way writes `first-run.txt` into the *profile*
  directory (a download replaces the preset directory by a rename and
  would take a marker there with it). A yes writes the starter profiles
  (`shader_source::DEFAULT_PROFILES`: CRT Aperture, CRT Royale, Apple
  II, each at the preset's own defaults, an empty override table) once
  the collection lands. `shader_library::create` deduplicates slugs, so
  re-running would make `crt-royale-2`, and a name already held is
  never written twice. The model holds the words at every step
  (`Message { step, headline, detail }`). A front end chooses only the
  buttons (Qt uses the platform's standard ones), which is why the size
  and destination are in the question. Accepting calls
  `editor::Presets::forget`, or the profile manager's cached "none"
  would go on offering what just arrived. `launcherx --first-run` and
  `--default-profiles` are the flow without a toolkit.
- **"Browse…" starts** at the field's own directory, else a suggestion
  (the collection, for a preset), else **the last directory any dialog
  browsed** (`browse::remember`, one line in `<data dir>/
  last-browse.txt`, `LAUNCHER_BROWSE_MEMORY` overrides), else the OS
  default. A Qt dialog given an empty folder opens in the working
  directory, which is why the memory exists. `launcherx --browse-start`
  prints the answer.
- **The preview is the player's picture.** The scale is the player's
  `floor(min(area/image)).max(1.0)`, integer, letterboxed and cropped
  like a window smaller than the mode. The `.max(1.0)` is load-bearing.
  Slang CRT presets assume they upscale, and some divide by zero when
  asked to shrink (`crt-aperture`'s `floor(OutputSize.y /
  SourceSize.y)`) and draw black. A source over 1600×1200 is resized on
  the CPU first (`MAX_SOURCE_W/H`), as a sanity cap.
- **The preview moves when the preset does**: interlacing, phosphor
  decay, NTSC shimmer. `preview::Preview::frame_interval` says how often
  to draw (`None` for a still preset) and the front end obeys (a QML
  `Timer`). The frame number comes from a clock at `FRAME_RATE` (60/s),
  not a count of renders, so the effect runs at the player's speed and
  drops frames rather than running slow. `shader_chain::
  preset_is_animated` looks for a *use* of `FrameCount` (the
  `params.FrameCount` access: 1131 slang-shaders passes declare it, 271
  read it) or a history/feedback texture, and errs towards animating.
  The headless verbs pin one frame (`PREVIEW_FRAME`, default 0) so their
  PNGs are reproducible. The `preview-anim` check covers it.

## Settings taxonomy

- **Per app:** the shader profiles and presets, the disc shelf, default
  hotkeys. There is no telemetry.
- **Per machine:** everything in the bundle (hardware, RAM, media,
  shader profile, pointer, the emulator's fast paths). The fast paths
  are per machine on purpose. Turning one off diagnoses *one guest*,
  and must not slow every other machine while that lasts.
- Bundles live in a plain, documented directory layout the user can
  back up.

## One front end over a core

The launcher is **one front end over one library** (ADR-014).
`launcher-qt/` on Qt 6 / QML through cxx-qt is a view over
`launcher-core/`, which has two more callers with no window:
`launcher-capi/` (the same models as a C ABI) and `launcherx` (every
toolkit-free debug verb, `launcher_core::cli`, which `launcher-qt`
answers identically). Why Qt is the one that ships is ADR-015, and why
the egui front end was deleted is ADR-017. The core did not shrink when
egui went, because the line is drawn at behaviour, not at how many front
ends there are.

### What is in the core, and why all of it

`launcher-core/` is **everything the launcher does that is not
drawing**, and the line sits deliberately far into what usually counts
as UI:

- the data: `bundle`, `library`, `disc_library`, `shader_profile` /
  `shader_library` / `shader_source`, `paths`;
- the machinery: `player` (spawning, `qemu-img`), `control` (QMP),
  `snapshots`, `preview`, `clone_machine`, `host_gpu`;
- **each window's own behaviour**: `machines`, `wizard`, `shelf`,
  `snaps`, `editor`, `firstrun`, down to the sentences they show, plus
  `browse` for the file-dialog decisions that are not a dialog, and
  `cli`.

The sentences are short and plain (user request): one or two per note,
in a user's words, with no dates, doc numbers, patch names or benchmark
stories. The *why* stays in the code and the docs.

ADR-014 records the drift that happened when each build held its own
copy of a window's state machine. None of it is expressible now. A
front end prints `ram_note()`, `accel_note()` and `network_notes()`,
fills a combo from `Family::ALL` / `CpuSpeed::ALL` and their
`label()`s, and a field with a consequence has only `choose_*`.

**What the core does not protect against.** A retained-mode front end
copies the model onto properties in a `publish()` some verb must call,
and a property nobody published stays at its default, which looks like
a real answer. Every Qt-only bug so far had a correct model, so a check
that asks the model passes on the broken build. The `qt-*` checks ask
the **window** what it shows. These rules came out of them:

1. **A QObject whose properties are read before any verb must publish
   in `cxx_qt::Initialize`**, the constructor QML uses. The profile list
   once offered "Download presets ()" into "" on a machine that had
   them, because nothing had published yet.
2. **A control that clamps is given its range before its value.** A
   `SpinBox` bounds a value against the range it has *at that moment*
   and never revisits it, so `ram_mb` published before
   `ram_min`/`ram_max` opened a new Win98 machine on 32 MB. Ranges are
   published first, `Wizard` publishes in `Initialize`, and `open` is
   published last, since that shows the window. Any `SpinBox` or
   `Slider` fed from properties is exposed the same way (`qt-wizard`
   compares what the memory field shows with the form).
3. **A window whose model flag drives it must not `close()` itself from
   its own `visibleChanged`.** Closing from the title bar re-entered
   `close()`. On Cocoa the outer close then skipped `endModalSession`,
   so the dialog vanished and the main window stayed locked (Linux never
   showed it). The guard is `closeIfShown` where the flag becomes a
   `close()`. The `closebox` probe sends a real close *event*
   (`src/close_event.cpp`) and `qt-close` wants exactly one.
4. **A component never writes the property its owner binds to, and a
   window never reaches for another window's model.** `PathField` once
   assigned its own `value`, which destroys the binding, so a second
   "New profile…" showed the last preset. It is now controlled: `value`
   in, `edited(path)` out, and the owner writes the model. A handler
   that called another window's `profiles.refresh()` threw a
   `TypeError` and took the `changed()` beside it along. Wiring between
   windows belongs in `Main.qml`, which owns both.
5. **A value that lives in two places needs one rule for which way it
   flows.** Typing does not break a binding (a keystroke is a C++ write;
   only a JavaScript write unbinds), so any verb that published while
   someone typed wrote the model's older text back into the field, and a
   typed name vanished when a combo box moved. Every form-changing verb
   goes through one `Wizard::edit(|form| …)` (pull, change, publish,
   never another order), and the shader editor's verbs call
   `ShaderEditor::catch_up` first (except `new_profile` and `edit`,
   where the model is deliberately newer). The `saveprofile` and
   `qt-wizard` probes type with `insert`, as a key press does.

### What the front end still owns

These are genuinely the toolkit's, and any other front end owes them
too:

| | Qt (`launcher-qt/`) |
|---|---|
| the file dialog | `QtQuick.Dialogs`; the desktop's own through Qt's platform theme, which on Linux `main.rs` names as the XDG portal's (a session Qt matches no theme to otherwise gets Qt's own picker) |
| when to redraw | a `Timer` per thing watched, off when idle |
| "the list changed" | `beginResetModel` / `dataChanged` |
| a destructive restore | a confirmation dialog |
| the preview frame | CPU readback → temp BMP → `Image` |
| secondary screens | real top-level windows |
| a headless frame | `QT_QPA_PLATFORM=offscreen` + `grabToImage` |

**The shader preview is the one place the Qt build is worse, and it is
fixable, in C++.** Qt Quick renders through QRhi and cxx-qt exposes no
handle to it, so `launcher-qt` opens a second, windowless wgpu device
(~40 MB of VRAM) and reads each frame back into a temp BMP. That costs
~3 ms of readback plus ~4 ms of write per 1280×960 frame, on every
slider drag (BMP because PNG took ~90 ms). The fix is a
`QQuickRhiItem` subclass importing the Vulkan image.

The shader manager is **two windows** (list and editor), not one that
resizes between modes, because a mapped window's size is the window
manager's to ignore (trap 4 below).

### The numbers

Measured 2026-09-06, after the split:

| | lines |
|---|---|
| `launcher-core/` | 4,435 |
| `launcher-qt/src/` (bridges only) | 2,132 |
| `launcher-qt/qml/` | 1,772 |
| dependencies beyond the core | `cxx-qt`, `cxx-qt-lib`, system Qt 6 |
| release binary | 24.4 MB plus ~38 MB of Qt runtime |

The split did not save lines (the core gained 2,469 new shared lines on
top of 1,966 moved). What it saves is having one place to change
anything. `launcher-qt` is outside the root workspace so a plain
`cargo build` never needs Qt 6 development files (the Mac, CI, the
Flatpak). Build it with `scripts/build.sh qt`, no CMake.

### Proving the core is the one implementation

- `--preview-shader` is `launcher_core::preview` on a headless device,
  the same code the Qt preview runs.
- `launcherx`, `launcher-core`'s own binary, is what `scripts/test.sh`
  and `tools/dos-guest-test.py` drive, so the suite builds no GUI
  toolkit to ask `--print-args` a question.
- `LAUNCHER_QT_SCREEN=create LAUNCHER_QT_ARG=dos:<name>` creates a
  machine through the real QML form offscreen, and its `machine.toml`
  is the model's (64 MB, `tcg`, no network, no tablet, `486dx2-66`).
  The screens and probes are listed in `docs/tracks/m6-launcher.md`.

## A third front end: `launcher-core` as a library

`launcher-capi/` is the C ABI that lets a front end in another language
be a view over the same models. It is shaped for a native macOS app in
Swift, which imports a C header with no bridge crate.

- `launcher-capi/include/launcher_core.h` is hand-written beside the
  code.
- Each window is an **opaque handle** (`lc_wizard_new` /
  `lc_wizard_free` …). Rows are addressed by index, one field at a time,
  exactly as the Qt build's `QAbstractListModel::data` reads them.
- Strings out are owned by the caller (`lc_string_free`) and never
  `NULL` for empty, so `NULL` means only "no such row".
- Nothing blocks on a guest. The long operations poll
  (`lc_snapshots_poll` while `lc_snapshots_job_pending`,
  `lc_editor_preset_state` during a download).
- It adds **no behaviour**. Every function is a thin wrapper.

It is a workspace member but not a default one (a `cdylib` and a
`staticlib` of the whole launcher). `scripts/build.sh` runs `cargo check
--release --workspace` so it cannot rot. `launcher-capi/examples/smoke.c`
is the smallest front end and a test (the `capi` check). What another
front end owes is the table above, and `lc_editor_read_frame` hands over
RGB8 for the preview.

## Shipping Qt

### What shipping Qt costs

Qt is a shared library, so every packager gained a job (ADR-015):

| package | how Qt gets there |
|---|---|
| Linux tarball (`scripts/package-linux.sh`) | not carried: a dependency on `qt6-base` + `qt6-declarative`, named by `install.sh` when the loader cannot find them |
| Flatpak (`packaging/flatpak/`) | the runtime **is** Qt: `org.kde.Platform` 6.10, the same freedesktop base |
| macOS (`scripts/package-macos.sh`) | `macdeployqt` before our own dylib closure, with `-qmldir=launcher-qt/qml` |
| Windows (`scripts/package-windows.sh`) | staged by hand: DLLs through the import walk, plus `plugins/`, `qml/` and a `qt.conf`; there is no cross `windeployqt` |

- **Our QML is compiled into the binary as a Qt resource**
  (`build.rs`'s `QmlModule`), so an installed launcher needs no `qml/`
  of its own. `macdeployqt` must be pointed at `launcher-qt/qml`, since
  its import scanner reads source. Without it the app dies on
  `module "QtQuick" is not installed`.
- **A package can pass every other check and open nothing.** Qt resolves
  its platform plugin and QML modules by name at run time, from
  directories no import table names. Each packager therefore opens a
  real window offscreen (`QT_QPA_PLATFORM=offscreen`,
  `LAUNCHER_QT_SHOT=<png>`) and requires the PNG. On macOS it does so
  under `DYLD_PRINT_LIBRARIES=1`, so the QML engine's images must be the
  app's own. The Flatpak's sandbox has its own `/tmp`, so its PNG goes
  under `$HOME`.

### Five Qt traps, each of which cost real time

1. **cxx-qt's generated setter skips the notify when the value already
   matches.** Writing `rust_mut().open = true` and then `set_open(true)`
   emits nothing, and every window opened once and stopped reacting.
   Keeping state in a core model *beside* the properties closes it. A
   `publish` writes every property through its setter, and the fields
   are never assigned anywhere else.
2. **`grabToImage` only works on an item the QML engine created.** A
   window's `contentItem`, `Overlay.overlay` and a `Popup`'s default
   `contentItem` are made in C++ and refuse silently, so the screenshot
   path grabs an item each window declares.
3. **`property var` holding a QObject gives QML no metadata**, so a
   binding on it is read once. Use the registered type (`property
   ShaderEditor editor`).
4. **A `Window`'s size cannot be changed after the window manager has
   mapped it.** The WM's own resize breaks a binding, and an assignment
   may be ignored. A window that wants two sizes is two windows.
5. **A `MessageDialog` cannot be driven from outside: `accept()` and
   `close()` both come back as `rejected()`.** A dialog whose visibility
   followed the model answered its own question. The first-run Yes
   started the download, the code closed the dialog, and the close
   arrived as "No" and put the offer away for good. Hence **one dialog
   per thing to answer, closed only by a press of its buttons**. The
   offer is `FirstRunDialog.qml` and `FirstRunResultDialog.qml`, and the
   download between them runs in the launcher's header. A probe presses
   a button by emitting `accepted` / `rejected`, never by calling the
   like-named methods.

### More Qt traps

- **Bindings and bridges.** A binding to a `Q_INVOKABLE` never
  re-evaluates. A combo box bound to a function keeps the list it was
  built with, so per-family lists are properties, and the optimization
  checkboxes bind one bitmask property (`optimizationsMask`; cxx-qt has
  no `QList<bool>`). A bridge method without `#[qinvokable]` is not
  callable from QML. The result is a `TypeError` in the log and a click
  that does nothing ("Turn all on / off" shipped that way; the `optall`
  probe clicks them now). `#[auto_cxx_name]` turns `d3d9_labels` into
  `d3D9Labels`, and QML cannot tell that a binding names a missing
  property, so the Direct3D row shipped as a label over an empty combo.
  The D3D9 properties name their `cxx_name`, and `qt-wizard` asks the
  combo's count and text. **A combo box with a `delegate` of its own is
  drawn by that delegate, not by the style.** The shader profile picker
  had one (a bare `ItemDelegate`, to map rows to ids in the window), so
  its list had no highlight, no hover and no bold current row while
  every other picker's did. Its rows now come from the model like every
  other picker's, and `qt-wizard` asks that combo too.
- **Text fields are two-way bound, so every verb that republishes the
  form pulls them first** (it goes through `edit`). `choose_section`
  once did not, and a page switch wrote the form's stale empty name over
  what the user had typed. `qt-wizard` pages away and back before it
  reads the name.
- **Esc.** Each secondary window binds Esc to `close()` with a
  `Shortcut`, which fired while that window's own file dialog was up (on
  macOS the dialog is a sheet and AppKit offers the key to the window
  under it). `PathField` publishes `browsing`, and the shelf, the form
  and the shader editor disable Esc while any `PathField` or their
  `FolderDialog` is open. A new dialog there joins that `enabled:`
  line. And **at most one visible window may have an armed Esc**. A
  transient window reports `isActive()` whenever its parent is, and two
  matches for one key are ambiguous, so neither fires. A window opened
  over another disarms the one under it. The `escfocus` probe and
  `qt-esc` want exactly one match (`src/focus_window.cpp` names the
  focus window, which `Window.active` cannot).
- **Native styles.** Never replace a Quick Controls control's
  `background` or `contentItem`. On macOS and Windows `appearance.cpp`
  keeps the native style, which refuses and warns for every instance.
  The lists are stock `ListView` + `ItemDelegate`. Quick Controls has
  no disclosure widget, so `launcher-qt/qml/Disclosure.qml` is ours (a
  rotating triangle, no tick). Nothing whose state is "showing /
  hidden" gets a checkbox, because a tick before "Emulation
  optimizations" said clearing it turns them off.
- **Layouts.** A layout row that can be empty beside a list says
  `Layout.fillHeight: false`. A nested layout whose children are all
  hidden has no maximum and takes a share of the spare height (the
  snapshots list stopped halfway, `qt-snapshots`). A grid column sized
  by `Layout.preferredWidth` alone moves with its text, so pin minimum =
  preferred = maximum and let one column take the spare width. Name a
  font family the platform has (Menlo / Consolas / `monospace`), or pay
  for a font-alias scan and a warning.
- **File dialogs.** An extension filter is case-sensitive on Linux, so
  `*.cue` hid `GAME.CUE`. `browse::extensions` gives each extension in
  both cases (not `[cC]`, which Windows and macOS dialogs do not take)
  and `PathField` hides the doubled list with `HideNameFilterDetails`. A
  mixed-case `.Cue` is still missed. A dialog's URL is not `file://` +
  a path. Stripping the prefix left `[` `]` percent-encoded ("Game
  [1996]" went on the shelf as `Game %5B1996%5D.iso`), so every dialog
  converts through `Browse.localPath` (`QUrl::toLocalFile`). `qt-shelf`
  asks the real dialog for `*.CUE` and picks `Game [1996].iso`.

## Platform packaging

### The names

The product is **2ksbox** (`2ksbox.com`, ADR-011) and the application ID
is **`com._2ksbox.Launcher`**: the desktop entry's filename, the icon's
name, the Wayland `app_id`, and the Flatpak and AppStream ID. The
underscore is required because no segment may start with a digit
(`flatpak build-init` rejects `com.2ksbox.…`). The user's data directory
`~/.local/share/2ksbox` was moved once from `win98-xp-virt`
(`launcher-core/src/paths.rs::data_dir`).

### The install layout

The launcher decides whether it is installed by looking at its own
executable (`launcher-core/src/paths.rs`). If `<exe dir>/..` contains
`share/2ksbox`, it is installed. Otherwise it finds everything in the
checkout it was built from (`target/`, `build/qemu`, `qemu/pc-bios`,
`guest-tools/out`, `third_party/`).

```
<prefix>/bin/2ksbox                            the launcher
<prefix>/bin/2ksbox-player                     the player
<prefix>/lib/2ksbox/libqemu-embed-i386.so
<prefix>/lib/2ksbox/…                          Glide wrapper, D3D executor + DXVK, wine/
<prefix>/libexec/2ksbox/qemu-img               ours, patched, kept off PATH
<prefix>/share/2ksbox/pc-bios/                 QEMU firmware (the player's -L)
<prefix>/share/2ksbox/guest-tools/             the guest-tools ISO
<prefix>/share/2ksbox/shaders/                 presets, when a package ships them
<prefix>/share/2ksbox/desktop/                 .desktop + metainfo, for install.sh
<prefix>/share/icons/hicolor/<n>x<n>/apps/     the application icon, every size
<prefix>/share/doc/2ksbox/                     COPYING, notices, README
```

Three rules hold it together:

- **Everything is relative to the executable**, so an extracted tarball
  works where it lands. The player finds `libqemu-embed` through an
  `$ORIGIN/../lib/2ksbox` rpath (`@loader_path` on macOS) ordered
  *before* the build-directory one, so a packaged binary never quietly
  loads a developer's library. The packaged player names the dlopened
  companions to QEMU itself (`player/src/companions.rs`,
  `player --companions`).
- **One layout or the other, never a mixture.** An installed launcher
  answers only with its own prefix, even for a file the package left
  out. A checkout fallback would let a broken package pass on the
  machine that built it. `LAUNCHER_*` overrides win over both.
- `qemu-img` is ours (patch 50's `cdimage` driver), so it lives in
  `libexec/`, where it neither shadows nor is shadowed by the system's.

On macOS the `.app`'s `Contents` is the prefix, with `MacOS/` doing
`bin/`'s job (`paths::bin_dir()`). Windows is flat. The launcher's
window carries the same identity: `app_id` = `com._2ksbox.Launcher`
through `QGuiApplication::setDesktopFileName`, and the icon through
`setWindowIcon` in `launcher-qt/src/window_icon.cpp` (cxx-qt-lib binds
`QImage` but not `QIcon`).

**The icon is one master and one generator.** `packaging/icon/
2ksbox.png` (a beige CRT showing a green hill under a teal sky) is
padded to 512×512, and every size is a downscale of that: 16–512 PNGs
and a four-size `.ico`, made by `scripts/gen-icons.sh`. All are checked
in, because nothing that needs one can draw it. `launcher-qt` embeds the
256 with `include_bytes!`, the Flatpak build is offline, the Windows
package is cross-built without ImageMagick, and a tarball's
`install.sh` has no tools. `gen-icons.sh --check` is the `icons` check.
Linux installs the set under `share/icons/hicolor/` and writes one
absolute path into the desktop entry's `Icon=` (a prefix outside
`XDG_DATA_DIRS` cannot resolve a theme name). macOS builds its `.icns`
from the same PNGs. On Windows the `.ico` goes *inside* every .exe as a
resource, the only thing Explorer reads. `packaging/windows/win-icon.rs`
is `include!`d by the build scripts of `launcher-qt` and `player` (a
shared file rather than a build-dependency, which would have to be
vendored into the Flatpak's offline sources). It writes a one-line
`.rc`, runs the container's `x86_64-w64-mingw32-windres` and links the
object. A host without windres gets a warning and an icon-less binary.
The loose `.ico` ships too, for shortcuts and installers.

**AppStream metadata** (`com._2ksbox.Launcher.metainfo.xml`, into
`share/metainfo`) carries a deliberately **empty** OARS rating. It rates
2ksbox itself, which has no chat, purchasing or user-to-user content.
The software someone runs in a guest is their own, the reading other
emulators apply. `appstreamcli validate --no-net` runs on every package
and fails on errors only. The one warning (no screenshots) needs
somewhere to host them.

### Per platform

- **Linux.** `scripts/package-linux.sh` stages the layout, asks the
  staged launcher and player where everything resolves with a scrubbed
  environment (`--paths`, `--companions`, a machine created and
  translated to a command line), and rolls a tarball. `install.sh`
  inside it copies the tree into a prefix. **The Flatpak**
  (`packaging/flatpak/`, `scripts/package-flatpak.sh`) is the primary
  Linux target (user decision: Flatpak first, then an AppImage). It is
  for bundling and distribution, not sandboxing. Our QEMU is a patch
  queue, so no distro `qemu` can be linked, the tarball ships none of
  the embed library's system libraries, and Flathub is where a stranger
  finds a Linux app. The sandbox is mostly nominal (`/dev/kvm`, the GPU,
  the network, and `--filesystem=host`, because bundles store absolute
  paths to discs and disks wherever a person keeps them). It builds
  from source (host binaries need a newer glibc than the runtime's),
  offline, reusing the layout through `package-linux.sh --prefix /app`,
  plus libslirp (absent from the runtime, and `-netdev user` needs it)
  and a build-only `distlib`.
- **macOS.** A signed, notarized .app with the JIT entitlement, native
  on Apple Silicon, carrying its whole non-system dylib closure (the Mac
  that runs it has no Homebrew and no Vulkan). There are **two builds
  (ADR-019)**: the App Store build and the community build. The recipe
  and reasoning are in `docs/build-macos.md` ("The app", "The floor").
- **Windows.** A portable zip, cross-built from Linux
  (`scripts/package-windows.sh`, `docs/build-windows.md`). Hardware
  acceleration is WHPX, stated beside the picker, with TCG as the
  fallback.

**Open:** Flathub (hosted screenshots on 2ksbox.com, and the manifest's
sources as git rather than a local directory), the AppImage (asked for,
not started), and a Windows installer.

## Out of scope for v1

Shared folders and drag-and-drop, clipboard sync, USB passthrough,
multi-monitor guests, and recording/streaming helpers. Recording pairs
naturally with the shader pipeline and is the first post-v1 candidate.
