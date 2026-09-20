/*
 * wavecaps.c — WAVECAPS.EXE: every multimedia device's name as a program
 * of the era reads it, raw, into C:\2KSBOX\WAVECAPS.LOG (guestlog.h).
 *
 * The four GetDevCaps calls hand back a fixed 32-byte szPname, and nothing
 * makes the driver terminate it. DirectX 9.0c's DSOUND.DLL copies the wave
 * names into a 32-byte stack buffer with a strcpy under a /GS cookie, so a
 * name that fills all 32 bytes kills the process with c0000409
 * (STATUS_STACK_BUFFER_OVERRUN) the moment anything enumerates DirectSound
 * — dxdiag, and every game. The Portuguese Windows 98's SB16 driver is one:
 * "Entrada de som wave da SB16 [220]" is 33 characters (doc 20 §5.3). So
 * every name is printed with where its NUL is (-1: none in 32 bytes) and
 * all 32 bytes in hex, which is the evidence: a screendump of Control
 * Panel shows the name, not whether it ends.
 *
 * A window-less program that writes a file: WIN.INI's run= starts it on a
 * guest with no shell to redirect (tools/win98-game-test.sh PULL=).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include "guestlog.h"

static FILE *f;

static void dump(const char *kind, UINT i, MMRESULT r, WORD mid, WORD pid,
                 const char *name)
{
    int k, nul = -1;

    fprintf(f, "%s %u: r=%u mid=%u pid=%u name=\"", kind, i, r, mid, pid);
    for (k = 0; k < 32 && name[k]; k++)
        fputc(name[k], f);
    if (k < 32)
        nul = k;
    fprintf(f, "\" nul_at=%d bytes:", nul);
    for (k = 0; k < 32; k++)
        fprintf(f, " %02x", (unsigned char)name[k]);
    fprintf(f, "%s\n", nul < 0 ? "  UNTERMINATED" : "");
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s)
{
    UINT i, n;

    f = guest_log_open("WAVECAPS.LOG", "w");
    if (!f)
        return 1;
    /* each struct is filled with 0xcc first, so bytes the driver never
     * wrote are told from ones it did */
    n = waveOutGetNumDevs();
    fprintf(f, "waveOut devices: %u\n", n);
    for (i = 0; i < n; i++) {
        WAVEOUTCAPSA w;
        MMRESULT r;
        memset(&w, 0xcc, sizeof w);
        r = waveOutGetDevCapsA(i, &w, sizeof w);
        dump("waveOut", i, r, w.wMid, w.wPid, w.szPname);
    }
    n = waveInGetNumDevs();
    fprintf(f, "waveIn devices: %u\n", n);
    for (i = 0; i < n; i++) {
        WAVEINCAPSA w;
        MMRESULT r;
        memset(&w, 0xcc, sizeof w);
        r = waveInGetDevCapsA(i, &w, sizeof w);
        dump("waveIn", i, r, w.wMid, w.wPid, w.szPname);
    }
    n = midiOutGetNumDevs();
    fprintf(f, "midiOut devices: %u\n", n);
    for (i = 0; i < n; i++) {
        MIDIOUTCAPSA m;
        MMRESULT r;
        memset(&m, 0xcc, sizeof m);
        r = midiOutGetDevCapsA(i, &m, sizeof m);
        dump("midiOut", i, r, m.wMid, m.wPid, m.szPname);
    }
    n = mixerGetNumDevs();
    fprintf(f, "mixer devices: %u\n", n);
    for (i = 0; i < n; i++) {
        MIXERCAPSA m;
        MMRESULT r;
        memset(&m, 0xcc, sizeof m);
        r = mixerGetDevCapsA(i, &m, sizeof m);
        dump("mixer", i, r, m.wMid, m.wPid, m.szPname);
    }
    fprintf(f, "end\n");
    fclose(f);
    return 0;
}
