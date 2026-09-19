/*
 * pwrprobe.c — the monitor power-down on demand, so that doc 19 §40 has a
 * test that does not wait on an idle timer.
 *
 * What went wrong without it: after the Control Panel's time-out Windows
 * asks the display driver for DPMS (`SETPOWERMANAGEMENT`), and a driver
 * that does not answer has the screen taken away from it instead — the
 * bare `switched out`, the frozen desktop and the band across its top.
 * Reproducing that meant leaving a machine alone for minutes and hoping
 * the power scheme in *that* image still had the time-out the last one
 * had; a 12-minute run that blanked nothing proved nothing either way
 * (2026-09-18).
 *
 * `SC_MONITORPOWER` is the same request the time-out makes, and any
 * process can make it: USER32 broadcasts it, and the shell turns it into
 * the driver escape (or, failing that, into the screen switch). So this
 * program *is* the time-out, minus the waiting.
 *
 * The evidence is in the QEMU log, not here. With the driver answering:
 *
 *     d3dpt9x: QUERYESCSUPPORT 00001804
 *     d3dpt9x: power state 00000004        (off; 1 = on)
 *
 * and no `d3dpt9x: switched out` anywhere. With a driver that does not
 * answer, there is a `switched out` instead and the adapter keeps its
 * linear mode — that run is the control, and it is what the band looked
 * like from the inside.
 *
 * `PWRPROBE [seconds]` powers the monitor off, waits (3 s by default),
 * asks for it back, and writes what it did to C:\PWRPROBE.LOG for the
 * harness to read back with mtools. Window-less, so WIN.INI's `run=`
 * starts it, like `ddprobe.exe` beside it.
 */

#include <windows.h>
#include <stdio.h>

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

    log_file = fopen("C:\\PWRPROBE.LOG", "w");
    say("pwrprobe: monitor off, %d s, then on", secs);

    /* The broadcast the idle time-out makes. SendMessage and not Post:
     * the answer is what says the shell took it, and a driver that
     * answers the escape is reached inside this call. */
    r = SendMessage(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, MON_OFF);
    say("pwrprobe: off -> %ld", (long)r);

    Sleep((DWORD)secs * 1000);

    r = SendMessage(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, MON_ON);
    say("pwrprobe: on -> %ld", (long)r);

    say("pwrprobe: done -- the verdict is the QEMU log's, not this file's");
    if (log_file) fclose(log_file);
    return 0;
}
