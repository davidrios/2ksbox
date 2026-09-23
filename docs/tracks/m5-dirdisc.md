# Track: M5g — a host directory as a CD-ROM (`isodir:`)

This track makes "share this folder with the guest" a disc in the drive.
`isodir:/path/to/folder` serves a host directory as a read-only
ISO 9660 + Joliet volume. libdisc generates the volume lazily. No image file
is written, nothing is copied, and `xorriso` is not needed at run time.

The track was opened and merged on 2026-09-06. On 2026-09-07 a step raised
the ceiling from a CD to a dual-layer DVD. This record keeps the scope, the
test loop, the traps and what stayed open. The design lives elsewhere:

- **Doc 17 §8:** the volume layout, the decisions, the limits and the
  model additions.
- **Doc 17 §5.1:** the `isodir` driver beside `cdimage`, and the `raw` node
  the block layer puts on top of it.
- **Doc 07:** the launcher side ("Add folder…", forced Insert/Eject).
- **`docs/tracks/m5-cdrom-backend.md`:** the M5 code that this track
  extends.

## Scope and files

- `libdisc/src/isodir.rs`: the layout builder.
- `libdisc/src/lib.rs`: `Source::Mem`, `eof_pad`, lazy payload handles,
  and `extent_at` as a binary search.
- `libdisc/src/msf.rs`: `Msf::from_lba` saturates.
- `libdisc/src/bin/discx.rs`: `selftest`'s `dirdisc` case, and `info` /
  `dump` / `convert` / `export` / `mktree` on a directory.
- `libdisc/qemu/cdimage.c`: the `isodir` BlockDriver. This is an overlay
  file of ours, so no QEMU patch was needed.
- QEMU patch 53 (`atapi-dvd-profile`): above an 80-minute CD the medium
  reports a DVD-ROM profile. Patch 52's shelf strips the prefix before it
  checks the host path.
- Launcher, shared with M6:
  - `launcher-core/src/disc_library.rs`: `qemu_medium`;
  - `launcher-core/src/bundle.rs`: comma doubling;
  - `launcher-core/src/control.rs`: forced insert;
  - `launcher-core/src/cli.rs`: `--discs publish`;
  - `launcher-qt/qml/DiscShelfWindow.qml`: "Add folder…".
- `libdisc.h` did not change: `libdisc_open` on a directory is the whole
  interface, so `LIBDISC_API_VERSION` stays 1.

## Test loop

```sh
scripts/build-libdisc.sh                        # cargo + relink QEMU
target/release/discx selftest build/test/disc   # includes the dirdisc case
target/release/discx info isodir:<dir>          # layout, size, mangled names
build/qemu/qemu-img info isodir:<dir>           # names isodir
scripts/test.sh all
```

| Check | What it proves |
|---|---|
| `dirdisc` | xorriso reads the fixture tree back identical. bsdtar with Joliet off reads the 8.3 tree, including which of two colliding names has which contents. `qemu-img convert` equals `discx export`. SeaBIOS probing with `CDIMAGE_TRACE=1` shows that the ATAPI path found the model (a plain `.iso` is the control). |
| `guest-dirdisc` | XP copies the extracted guest-tools directory, served as a folder, through cdrom.sys, and every file matches. |
| `dirshelf` | The launcher's headless verbs put three folders on the shelf, under their own names: one with a space, one with a comma, one plain. The shelf file and `--print-args` say `isodir:`, commas are doubled, and our QEMU opens both. |

Local tools:

- `tools/dirdisc-guest-test.sh <image> [win98|xp]`: a folder read with the
  guest's own `dir` and `type`.
- `BIG=1` with the same tool: marker files read back at 703 MiB, 878 MiB,
  2 GiB, 4 GiB and 7.8 GiB. Both Win98 and XP read all five.
- `tools/xp-cdimage-test.sh <image> isodir:<dir> <dir>`: a round trip.
- `tools/cdshelf-guest-test.sh`.

Harness traps found here (mtools' hidden-sectors field, XP's lazy writer,
the macOS tool differences, NFD names from bsdtar) are in
`docs/testing.md`.

## Traps

- **The `raw` node hides failures.** The block layer puts a `raw` format
  node above a protocol driver it found by prefix, so `cdimage_disc()` has
  to walk down through format nodes. If it does not, nothing fails
  visibly: files still read, and only the model's answers go missing (the
  TOC, READ CD, the sense of a bad sector). The SeaBIOS probe in `dirdisc`
  is the proof, and it needs no guest.
- **A prefix is a contract.** Everything that *inspects* a medium string
  has to understand `isodir:`, not only the code that passes it on. The C
  side of the shelf once `access()`ed the prefix with the path and called
  every folder "missing on the host".
- **Validate where the disc is built.** Make the arithmetic after that
  unable to fail. A 34 GiB folder once got past the builder and panicked
  in MSF on the TOC's lead-out. The panic reached the guest as
  `LIBDISC_EIO` on an unrelated command. Now the size is refused up front
  with both sizes in the message, and `Msf::from_lba` saturates.
- **Force tray changes.** An unforced `blockdev-change-medium` or `eject`
  only asks a guest that has locked the tray. XP locks it for every open
  handle, so the swap happened minutes later. The launcher forces it (doc
  07).
- **A program that polls a drive consumes its media-change news.** The
  sense is delivered once, to whoever asks first. `CDSHELF` therefore
  dismounts the volume itself (`FSCTL_DISMOUNT_VOLUME`) after a swap.
  Before that, a swap into a full drive left Windows on the old disc.
- **Changing a host file under a mounted disc gives a read error** (the
  sector reads as `EMEDIUM`), never a torn file. To see edits, eject and
  insert again.

## What stayed open

- **The stale-file rule is checked on the host only**, through
  `discx selftest`'s EMEDIUM case. A guest version would race XP's
  read-ahead and be flaky rather than stronger.
- **No MSCDEX run.** The FreeDOS floppy the DOS tools use has no CD
  driver. bsdtar with Joliet off stands in for it.
- **Not bootable.** There is no El Torito catalogue (doc 17 §8 says how it
  would be added).
