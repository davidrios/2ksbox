# The Windows 9x display-driver interface headers

The 16-bit GDI display-driver, DIB Engine, mini-VDD and DirectDraw HAL
interface definitions the Win98/Me driver (`../w9x/`, doc 19) builds
against. They do for `w9x/` what `../ddk/` does for the XP driver, and
for the same reason: **the Microsoft DDK is not used here.**

| File | What it defines |
|---|---|
| `gdidefs.h`, `dibeng.h`, `valmode.h`, `winhack.h` | the GDI display-driver and DIB Engine interfaces, mode validation |
| `minivdd.h`, `vmm.h` | the mini-VDD's entry points and the ring-0 VMM services, structures and control messages |
| `ddrawi.h`, `dmemmgr.h` | the DirectDraw driver interface the 16-bit half publishes its HAL through: the `DDHALINFO` it builds, the `DCICMD` escape it answers, the `DD32BITDRIVERDATA` naming the ring-3 DLL (doc 19 §2) |
| `dibeng.def`, `dibeng.lbc` | the DIB Engine's exports; `wlib` turns the `.lbc` import list into the `DIBENG.DLL` import library, so nothing from a DDK is needed to link against it |

All of them are taken verbatim from JHRobotics'
[`vmdisp9x`](https://github.com/JHRobotics/vmdisp9x) (MIT, © 2022–2023
JHRobotics / Jaroslav Hensl, deriving from Michal Necasek's Win9x video
minidriver). Its `ddk/` directory carries no Microsoft code or
copyright: the files are interface descriptions written from the
published documentation. MIT is compatible with this repository's
GPL-2.0-or-later; the licence text is in that project's `LICENSE` and
the notice is kept here.
