#!/usr/bin/env python3
"""The gameport as a *guest* meets it (M13 path B,
docs/tracks/m13-gamepads.md): a DOS program arms the one-shots at 0x201
and counts them down while the host moves a scripted pad — and the counts
it prints over COM1 are the evidence.

    tools/pad-guest-test.py             # needs nasm, mtools, build/qemu
    tools/pad-guest-test.py --verbose   # ...and every sample line
    UNTHROTTLED=1 tools/pad-guest-test.py   # the control: no -icount

`UNTHROTTLED=1` is the control the track doc asks for, and it is worth
running once to see the answer rather than to pass: with the pacing off
the same stick position counts about ten times higher (a guest running at
whatever speed the host gives it) and wanders by half from sample to
sample. That is the risk path B carries — a game with a fixed timeout
count sees a stick jammed at one end — and the reason the DOS family runs
`-icount shift=N,align=on` anyway, for its processor picker. Nothing in
the model changes between the two runs; only the machine does.

**It must be the player, not `qemu-system-i386`.** The pad reaches the
guest through the embed library (`qemu_embed_pad_state` -> the input
bottom half -> `gameport_set_state`), and only the player drives that; a
bare QEMU has a gameport that nothing ever moves. `PLAYER_PAD_SCRIPT`
stands in for the controller, so this runs on a machine with nothing
plugged in — which is the whole reason that source exists.

What it proves, and why each piece is here rather than in the `pad` check
in `scripts/test.sh` (which reads the port from the monitor and can prove
presence, arming and expiry, but cannot move a pad and has no guest):

  * the port is *found* — an idle read of 0xf0, not the 0xff an absent
    port gives off the open bus;
  * an axis is a **count**, and the count follows the stick: the left
    extreme is a ~24 us pulse and the right a ~1124 us one, so the two
    differ by tens of times and the centre sits about half way;
  * the **d-pad reaches the port at all** — nothing in the script touches
    `ly`, so every reading on the Y axis comes from the hat being folded
    onto it in the device (`gameport_set_state`), which is what makes a
    digital pad work on a port that has only pots;
  * the axes are independent: the script never touches the second stick's
    Y, and the fourth count must not move;
  * the buttons are the four face buttons, low while held.

The phases are deliberately *distinguishable by value*, so nothing here
has to align the guest's clock with the host's frame counter: the stick
only ever moves X, the d-pad only ever moves Y, the second stick only
ever moves Z. The order the samples arrive in does not matter.

Outputs in build/pad-guest/. Local only (it fetches the FreeDOS floppy
`tools/x87-guest-test.py` uses and it wants a display for the player's
window), not wired into scripts/test.sh.
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLAYER = os.path.join(ROOT, "target/release/player")
OUT = os.path.join(ROOT, "build/pad-guest")

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

ASM = os.path.join(ROOT, "guest-tools/src/padtest.asm")

# The pad, as frames of the player's own publish loop — which is what the
# scripted source counts, so these are *not* seconds. Measured here: the
# player publishes about 60 a second while the guest is printing (which it
# is: every sample goes to the VGA text screen as well as to COM1, so
# there is always a changed surface to publish).
#
# One control per axis, by design (see the module docstring): `lx` for X,
# the d-pad for Y, `rx` for Z, and the fourth axis is never touched.
#
# Two things about the shape, both learned the first time this ran:
#
#   * it starts LATE. FreeDOS is at the prompt in about four seconds, but
#     the pad script starts at frame 1 — the very first poses played to a
#     machine that was still booting, and X's short end was simply never
#     sampled. The offset is the fix, and generous.
#   * it REPEATS. Two cycles, so a slower boot or a host publishing at
#     half the rate still leaves every pose inside the guest's sampling
#     window, and the harness stops as soon as it has seen them all.
PHASE = 150             # frames a pose is held: ~2.5 s, about ten samples
OFFSET = 600            # ...after a first pass of ten seconds, for the boot
CYCLES = 2

POSES = [
    "lx=-1.0",          # X to the short end
    "lx=1.0",           # X to the long end
    "lx=0.0",           # X centred
    "dpad_up=1",        # Y short — through the fold, not through `ly`
    "dpad_up=0,dpad_down=1",
    "dpad_down=0,rx=1.0",   # the second stick's X
    "rx=0.0,south=1",       # button 1
    "south=0,east=1",       # button 2
    "east=0",
]


def script():
    steps = []
    frame = OFFSET
    for _ in range(CYCLES):
        for pose in POSES:
            for item in pose.split(","):
                steps.append("%d:%s" % (frame, item))
            frame += PHASE
    return ",".join(steps)


SCRIPT = script()


def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def build():
    """PADTEST.COM on a FreeDOS floppy that runs it from AUTOEXEC."""
    com = os.path.join(OUT, "PADTEST.COM")
    img = os.path.join(OUT, "pad.img")
    sh("nasm", "-f", "bin", "-o", com, ASM)
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nPADTEST.COM\r\n")
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::PADTEST.COM")
    return img


def run(img, log, plog):
    """The player, with a gameport and a scripted pad, on the floppy."""
    for f in (log, plog):
        if os.path.exists(f):
            os.unlink(f)
    env = dict(os.environ, PLAYER_PAD="gameport", PLAYER_PAD_SCRIPT=SCRIPT)
    # A period-paced machine, because that is what the port is for: with
    # `-icount shift=N,align=on` the guest's counting loop and our
    # one-shot are paced by one clock, which is the arrangement a game
    # with a fixed timeout needs (docs/tracks/m13-gamepads.md, path B's
    # risk). The unthrottled control is what UNTHROTTLED=1 runs.
    icount = [] if os.environ.get("UNTHROTTLED") else ["-icount", "shift=7,align=on"]
    with open(plog, "wb") as out:
        p = subprocess.Popen([
            PLAYER, "--",
            "-L", os.path.join(ROOT, "qemu/pc-bios"),
            "-machine", "pc", "-cpu", "486", "-m", "16",
            "-vga", "cirrus", "-net", "none",
            "-audiodev", "none,id=a0",
            "-device", "gameport",
            "-drive", "file=%s,if=floppy,index=0,format=raw" % img,
            "-boot", "a", "-serial", "file:" + log, *icount,
        ], env=env, stdout=out, stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        while time.time() - t0 < 300:
            time.sleep(1)
            if p.poll() is not None:
                break
            if not os.path.exists(log):
                continue
            text = open(log, "rb").read().decode("latin-1")
            # Every pose seen: stop the machine rather than sit through
            # the rest of the guest's sampling window. The guest's own
            # DONE is the other end — a run that never satisfies the
            # checks plays out in full and then fails with the counts.
            if check(text, quiet=True) or "DONE" in text:
                break
        else:
            raise SystemExit("timeout waiting for DONE (see %s)" % log)
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
    return open(log, "rb").read().decode("latin-1")


def samples(text):
    """The `X=n Y=n Z=n R=n B=xx` lines, as dicts.

    A line that does not parse is skipped in silence, because the log is
    read while the guest is still writing it and the last line is
    routinely half a sample.
    """
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("X=") or " B=" not in line:
            continue
        try:
            f = dict(p.split("=", 1) for p in line.split(" ") if "=" in p)
            row = {k: int(v, 16 if k == "B" else 10) for k, v in f.items()}
        except ValueError:
            continue
        if set(row) >= {"X", "Y", "Z", "R", "B"}:
            out.append(row)
    return out


def check(text, quiet=False):
    """Every assertion about the port, over whatever samples have arrived.

    `quiet` is what the run loop polls with while the guest is still
    going: the same checks, so the run can stop the moment the pad has
    been all the way round rather than waiting out the guest's window.
    """
    ok = True
    say = (lambda *a: None) if quiet else print
    rows = samples(text)
    if "NOPORT" in text:
        say("FAIL the guest found no gameport at 0x201 (an idle read had an axis bit set)")
        return False
    if len(rows) < 20:
        say("FAIL only %d samples came back; the guest never really ran" % len(rows))
        return False
    if "CAPPED" in text:
        say("FAIL a counting loop hit its cap: an armed one-shot never expired")
        ok = False

    def span(axis):
        vs = [r[axis] for r in rows]
        return min(vs), max(vs)

    xlo, xhi = span("X")
    ylo, yhi = span("Y")
    zlo, zhi = span("Z")
    rlo, rhi = span("R")
    say("counts over %d samples: X %d..%d  Y %d..%d  Z %d..%d  R %d..%d"
          % (len(rows), xlo, xhi, ylo, yhi, zlo, zhi, rlo, rhi))
    say("button nibbles seen: %s" % " ".join(sorted({"%02x" % r["B"] for r in rows})))

    # The fourth axis is the one nothing in the script touches, so its own
    # spread is this guest's measurement noise — and every other axis is
    # judged against *that* rather than against a number chosen here.
    #
    # It has to be, because the noise is a property of the machine and not
    # of the port: a count is loop iterations, so it depends on how fast
    # the guest happens to be running. Paced (`-icount align=on`, what a
    # DOS machine gets) the same stick position comes back within a few
    # per cent; unpaced it wanders by half, because the host is deciding
    # the guest's speed from moment to moment. Both are seen here, and
    # only the second needs slack.
    jitter = rhi / max(rlo, 1)
    say("the undriven axis's own spread — this guest's loop jitter: %.2fx" % jitter)
    if jitter > 3.0:
        say("FAIL R spans %d..%d with nothing driving it: that is not jitter" % (rlo, rhi))
        ok = False
    # The stick, on X. Its two extremes are a 24 us pulse and a 1124 us
    # one, so the true ratio is about 46; ten is slack, and it must also
    # stand well clear of the noise floor above — a model that drove all
    # four one-shots off one axis would move X and R together and pass a
    # fixed threshold.
    if xhi < max(10, 4 * jitter) * max(xlo, 1):
        say("FAIL X barely moved (%d..%d): the stick is not reaching the port" % (xlo, xhi))
        ok = False
    # ...and the centre, which is what a game calibrates against: 576 of
    # 1124 us, so about half of the long end.
    if not any(0.35 * xhi <= r["X"] <= 0.7 * xhi for r in rows):
        say("FAIL no sample had X near the middle of its range: a centred stick did not read centred")
        ok = False
    # The d-pad, on Y — nothing in the script moves `ly`, so this is the
    # fold in gameport_set_state and nothing else.
    if yhi < max(10, 4 * jitter) * max(ylo, 1):
        say("FAIL Y barely moved (%d..%d): the d-pad is not reaching the axes" % (ylo, yhi))
        ok = False
    # The second stick, on Z: centre to the long end only, so ~1124/576.
    if zhi < 1.4 * jitter * max(zlo, 1):
        say("FAIL Z barely moved (%d..%d): the second stick is not reaching the port" % (zlo, zhi))
        ok = False

    # Buttons: low while held, and all four high with nothing pressed.
    seen = {r["B"] & 0xf0 for r in rows}
    for bit, name in ((0x10, "button 1 (south)"), (0x20, "button 2 (east)")):
        if not any(b & bit == 0 for b in seen):
            say("FAIL %s never read as pressed" % name)
            ok = False
    if 0xf0 not in seen:
        say("FAIL no sample had every button released")
        ok = False
    return ok


def main():
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(PLAYER):
        raise SystemExit("no %s — cargo build --release" % PLAYER)
    # nasm, mtools and the FreeDOS floppy, the same ones the x87 and MIDI
    # batteries use (fetched once into build/images/).
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    img = build()
    log = os.path.join(OUT, "serial.log")
    plog = os.path.join(OUT, "player.log")
    t0 = time.time()
    text = run(img, log, plog)
    print("%.0f s in the guest; %s" % (time.time() - t0, log))
    if "--verbose" in sys.argv:
        for line in text.splitlines():
            if line.strip():
                print("   ", line.strip())
    ok = check(text)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
