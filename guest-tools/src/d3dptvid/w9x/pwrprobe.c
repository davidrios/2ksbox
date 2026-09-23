/*
 * pwrprobe.c: the monitor power-down on demand, so doc 19 §41 has a test
 * that does not wait on an idle timer.
 *
 * After the Control Panel's time-out Windows asks the display driver for
 * DPMS (`SETPOWERMANAGEMENT`). A driver that does not answer has the
 * screen taken away from it instead, which gives the bare `switched out`,
 * the frozen desktop and the band across its top. Waiting for the idle
 * time-out depends on each image's power scheme, and a run that blanks
 * nothing proves nothing.
 *
 * `SC_MONITORPOWER` is the same request the time-out makes, and any
 * process can make it. USER32 broadcasts it, and the shell turns it into
 * the driver escape (or, failing that, into the screen switch). So this
 * program is the time-out without the wait.
 *
 * The evidence is in the QEMU log, not here. With the driver answering:
 *
 *     d3dpt9x: QUERYESCSUPPORT 00001804
 *     d3dpt9x: power state 00000004        (off; 1 = on)
 *
 * and no `d3dpt9x: switched out` anywhere. With a driver that does not
 * answer, there is a `switched out` instead and the adapter keeps its
 * linear mode. That run is the control, and it is what the band looks
 * like from the inside.
 *
 * `PWRPROBE [seconds]` powers the monitor off, waits (3 s by default),
 * asks for it back, and writes what it did to C:\2KSBOX\PWRPROBE.LOG
 * (guestlog.h) for the harness to read back with mtools. It has no window,
 * so WIN.INI's `run=` starts it, like `ddprobe.exe`.
 */

#include <windows.h>
#include <stdio.h>
#include "../../guestlog.h"

#ifndef SC_MONITORPOWER
#define SC_MONITORPOWER 0xF170
#endif

/* The three states the broadcast takes: the DDK's VideoPowerState minus
 * the ones USER never sends. -1 is "on", which is what a keypress does. */
#define MON_ON      (-1)
#define MON_LOWPOWER  1
#define MON_OFF       2

static FILE *log_file;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (log_file) {
        vfprintf(log_file, fmt, ap);
        fputc('\n', log_file);
        fflush(log_file);
    }
    va_end(ap);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    int secs = 3;
    LRESULT r;

    (void)inst; (void)prev; (void)show;
    if (cmd && *cmd) {
        int n = atoi(cmd);
        if (n > 0 && n < 300) secs = n;
    }

    log_file = guest_log_open("PWRPROBE.LOG", "w");
    say("pwrprobe: monitor off, %d s, then on", secs);

    /* The broadcast the idle time-out makes. SendMessage, not Post, because
     * the answer says the shell took it, and a driver that answers the
     * escape is reached inside this call. */
    r = SendMessage(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, MON_OFF);
    say("pwrprobe: off -> %ld", (long)r);

    Sleep((DWORD)secs * 1000);

    r = SendMessage(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, MON_ON);
    say("pwrprobe: on -> %ld", (long)r);

    say("pwrprobe: done -- the verdict is the QEMU log's, not this file's");
    if (log_file) fclose(log_file);
    return 0;
}
