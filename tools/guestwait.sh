# guestwait.sh — the guest test tools' waits, event-driven. Source it:
#
#   . "$ROOT/tools/guestwait.sh"
#
# These tools used to open with a fixed `sleep $BOOT_WAIT` of 45 to 180
# seconds, sized by hand for the slowest machine anyone had run them on. A
# fixed sleep is wrong in both directions: it burns the difference on every
# faster host (measured 2026-09-07 on the Air under TCG, fresh overlays:
# XP's Run dialog took the first command at ~26 s against sleeps of 45, 60
# and 120; Win98's at ~23 s against 150 and 180 — and both are far quicker
# under KVM on the rig, where the sleeps did not change), and it fails
# outright on a host that is slower that day, which is exactly when a test
# should still pass. Everything here waits for something the guest actually
# did instead, and takes its old BOOT_WAIT as a *cap* — the number now only
# decides when to give up.
#
# No screendumps anywhere: a screendump proves a surface was drawn, not
# that a guest is alive (docs 19 §15 and the blinking-caret gotcha in
# CLAUDE.md — `vga_draw_text` keeps drawing over a dead machine). The
# signals, weakest to strongest:
#
#   gw_wait_log    (and gw_wait_count) a line our own device wrote (`d3dpt-vga: linear mode
#                  on`, the display driver programming the desktop mode:
#                  9 s into an XP boot here). A lower bound, and only on
#                  the machines that have our adapter.
#   gw_wait_quiet  the guest stopped reading its disk (QMP query-blockstats).
#                  Adapter-agnostic, for the cirrus machines that have no
#                  serial line — an approximation, not proof.
#   gw_poke_until  the guest ran a command and said so on COM1 (or wherever
#                  the caller looks). The only one that proves the shell can
#                  take a command, and the tools have to do the poke anyway.
#
# What this actually buys, measured 2026-09-07 on the Air (TCG, fresh
# overlays, against sleeps of 45 to 180): XP from QEMU's start to a guest
# that has run a command, 36-46 s; Win98, ~118 s — the guest itself is
# there at ~23 s, but QMP is starved of the lock while the vCPU translates
# a boot, so the knocking is slower than the guest. Both numbers are of a
# machine that answered, which no sleep can tell you.
#
# Every verb prints what it waited for, and for how long, on stderr, so a
# run's log says where its time went. Set GW_PID=<qemu pid> after launching
# and every wait also gives up the moment the guest exits, instead of
# sitting out the whole cap on a QEMU that died at once.

GW_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GW_PID="${GW_PID:-}"

gw_say() { echo "guestwait: $*" >&2; }

gw_qmp() { python3 "$GW_ROOT/tools/qmpc.py" "$@" >/dev/null 2>&1; }

# the guest is gone: nothing is ever going to answer, stop waiting
gw_dead() { [ -n "$GW_PID" ] && ! kill -0 "$GW_PID" 2>/dev/null; }

gw_wait_sock() {  # <sock> [cap=60] — QEMU is up far enough to talk to
  local sock=$1 cap=${2:-60} t0 t
  t0=$(date +%s)
  while [ ! -S "$sock" ]; do
    gw_dead && { gw_say "QEMU exited before its QMP socket appeared"; return 1; }
    t=$(( $(date +%s) - t0 ))
    [ "$t" -ge "$cap" ] && { gw_say "no QMP socket after ${t}s ($sock)"; return 1; }
    sleep 0.2
  done
  return 0
}

gw_wait_log() {  # <file> <pattern> [cap=300] — a line in a log the guest or QEMU writes
  local log=$1 pat=$2 cap=${3:-300} t0 t
  t0=$(date +%s)
  while :; do
    if grep -q "$pat" "$log" 2>/dev/null; then
      gw_say "'$pat' after $(( $(date +%s) - t0 ))s"
      return 0
    fi
    gw_dead && { gw_say "the guest exited waiting for '$pat'"; return 1; }
    t=$(( $(date +%s) - t0 ))
    [ "$t" -ge "$cap" ] && { gw_say "timed out after ${t}s waiting for '$pat'"; return 1; }
    sleep 1
  done
}

gw_wait_count() {  # <file> <pattern> <n> [cap=300] — the pattern's nth occurrence
  # A reboot is two of the same line: the display driver programs the mode
  # once on the way up and once more after the restart, and the second one
  # is the only proof the machine really came back (a screendump of the
  # first desktop looks exactly the same).
  local log=$1 pat=$2 n=$3 cap=${4:-300} t0 t c
  t0=$(date +%s)
  while :; do
    c=$(grep -c "$pat" "$log" 2>/dev/null || true); c=${c:-0}
    if [ "$c" -ge "$n" ]; then
      gw_say "'$pat' x$n after $(( $(date +%s) - t0 ))s"
      return 0
    fi
    gw_dead && { gw_say "the guest exited waiting for '$pat' x$n (saw $c)"; return 1; }
    t=$(( $(date +%s) - t0 ))
    [ "$t" -ge "$cap" ] && { gw_say "timed out after ${t}s waiting for '$pat' x$n (saw $c)"; return 1; }
    sleep 1
  done
}

gw_wait_quiet() {  # <sock> [cap=300] [still=8] — the disks stop being read
  # For a guest with neither our adapter nor a serial line. Boot reads are
  # bursty right to the end (XP here: a last 354-operation burst at 33 s,
  # then nothing), so "quiet" means no read for `still` seconds running,
  # and the caller still has to poke and check afterwards.
  local sock=$1 cap=${2:-300} still=${3:-8}
  python3 - "$sock" "$cap" "$still" "${GW_PID:-0}" <<'PY'
import json, os, socket, sys, time
sock, cap, still, pid = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
def alive():
    if not pid:
        return True
    try:
        os.kill(pid, 0); return True
    except OSError:
        return False
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock)
f = s.makefile("rwb", buffering=0)
json.loads(f.readline())
def cmd(e):
    f.write((json.dumps({"execute": e}) + "\n").encode())
    while True:
        r = json.loads(f.readline())
        if "event" not in r:
            return r
cmd("qmp_capabilities")
t0 = time.time(); last = None; quiet_since = None
while time.time() - t0 < cap:
    rd = sum(d["stats"]["rd_operations"] for d in cmd("query-blockstats")["return"])
    now = time.time()
    if rd != last:
        last, quiet_since = rd, now
    elif quiet_since and now - quiet_since >= still:
        print("guestwait: the disks went quiet after %.0fs" % (now - t0), file=sys.stderr)
        sys.exit(0)
    if not alive():
        print("guestwait: the guest exited while its disks were being watched", file=sys.stderr)
        sys.exit(1)
    time.sleep(1)
print("guestwait: the disks never went quiet in %.0fs" % cap, file=sys.stderr)
sys.exit(1)
PY
}

gw_run_dialog() {  # <sock> <family> — dismiss whatever is up, open Run, clear it
  local sock=$1 family=$2
  gw_qmp "$sock" keys ret; sleep 1          # a message box, if any
  gw_qmp "$sock" keys esc; sleep 1          # and whatever it left focused
  if [ "$family" = win98 ]; then
    gw_qmp "$sock" keys ctrl+esc; sleep 3; gw_qmp "$sock" keys r
  else
    gw_qmp "$sock" keys meta_l+r
  fi
  sleep 3
  # the first key after the chord is lost, and Run opens with its last
  # command selected: a space and a backspace absorb the one and clear the
  # other, whichever of the two actually happened
  gw_qmp "$sock" type ' '; gw_qmp "$sock" keys backspace
}

gw_poke_until() {  # <sock> <family> <cmd> <cap> <ready...> — until the guest answers
  # Opens the Run dialog, types the command, and waits to see whether the
  # guest did anything about it; on a boot that is not there yet nothing
  # was typed anywhere, so it costs a keystroke and is tried again. This is
  # the tools' own retry loop (they all needed one), started at second zero
  # rather than after a sleep long enough to make the first try succeed.
  local sock=$1 family=$2 cmd=$3 cap=$4; shift 4
  local t0 t n=0 i
  t0=$(date +%s)
  while :; do
    # a knock that landed while the last one was still being typed
    if [ "$n" -gt 0 ] && "$@" >/dev/null 2>&1; then
      gw_say "the guest answered after $(( $(date +%s) - t0 ))s (poke $n)"
      return 0
    fi
    n=$(( n + 1 ))
    gw_run_dialog "$sock" "$family"
    gw_qmp "$sock" type "$cmd"
    gw_qmp "$sock" keys ret
    # The answer is checked every second; this only decides when to knock
    # again. Keep it generous: during a TCG boot the QEMU main loop is
    # starved of the lock by the vCPU, so a knock's own keystrokes can take
    # tens of seconds to go through, and a guest that took the last one is
    # better waited on than typed at twice.
    for i in $(seq 1 "${GW_POKE_EVERY:-12}"); do
      sleep 1
      if "$@" >/dev/null 2>&1; then
        gw_say "the guest answered after $(( $(date +%s) - t0 ))s (poke $n)"
        return 0
      fi
    done
    gw_dead && { gw_say "the guest exited after $n pokes"; return 1; }
    t=$(( $(date +%s) - t0 ))
    if [ "$t" -ge "$cap" ]; then
      gw_say "no answer from the guest in ${t}s ($n pokes) — it may never have reached its shell"
      return 1
    fi
  done
}

gw_wait_exit() {  # [pid=$GW_PID] [cap=120] — the guest finished shutting down
  local pid=${1:-$GW_PID} cap=${2:-120} t0 t
  [ -n "$pid" ] || return 0
  t0=$(date +%s)
  while kill -0 "$pid" 2>/dev/null; do
    t=$(( $(date +%s) - t0 ))
    [ "$t" -ge "$cap" ] && { gw_say "the guest was still running ${t}s after it was asked to stop"; return 1; }
    sleep 1
  done
  gw_say "the guest shut down after $(( $(date +%s) - t0 ))s"
  return 0
}
