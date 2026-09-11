# SeaBIOS patch queue (the VGA BIOS)

Patches to the SeaBIOS tree QEMU 9.2.4 pins (`qemu/roms/seabios`,
rel-1.16.3), applied by `scripts/build-vgabios.sh` to a fresh copy of it
(never to the submodule), which then builds the VGA BIOS for the two
variants a 2ksbox machine loads: `stdvga` (`-vga std`, and `d3dpt-vga`,
whose `romfile` it is) and `cirrus`. The results are
`firmware/vgabios-stdvga.bin` and `firmware/vgabios-cirrus.bin`, and
`scripts/prepare-qemu.sh` copies them over `qemu/pc-bios/`, so every
package ships them.

**The blobs are checked in**, and rebuilt and committed together with any
change here. SeaBIOS is 16-bit x86 code that needs gcc (`-m32`) and GNU
ld; the Linux box has them, the Mac (clang, Apple ld) and the Flatpak SDK
do not, and the Windows build takes its firmware from the same tree.
QEMU does the same for its own `pc-bios/`. A rebuild on another compiler
is **not** byte-identical (QEMU's own prebuilt ROM and a pristine build
here differ from byte 5), so the evidence for a blob is the guest check,
not a hash: `vbe-palette` in `scripts/test.sh`. The version string is
`rel-1.16.3-0-g<commit>-2ksbox`, with no build time or host name in it.

Patches are git-format diffs relative to the SeaBIOS tree
(`git diff --no-prefix --no-index a/<file> b/<file>` from two copies),
applied with `patch -p1`.

| Patch | What / why | Drop when |
|---|---|---|
| `01-vbe-set-palette` | VBE function 09h, *Set/Get Palette Data*: BL 00h/80h set, 01h get, CX entries from DX, each blue/green/red/alignment in the DAC's current width, through the VGA DAC ports; a secondary palette (02h/03h) and direct-colour modes are refused. SeaBIOS has no 09h case at all (`debug_stub`, `AX=0100`) and yet reports every VBE mode as "not VGA compatible", which is what tells a program to use this function: DOS Quake quits at 640x480 and above with "Unable to load VESA palette", Duke Nukem 3D's VESA modes draw in the default colours (docs/00-status.md) | SeaBIOS implements 4F09h and QEMU ships it |
