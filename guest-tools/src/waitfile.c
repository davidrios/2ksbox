/*
 * waitfile.c: WAITFILE.EXE, wait for a file to appear, then start something.
 *
 * A harness that starts a game at login has to let whatever else runs at
 * login finish first. On a machine with the emulated Voodoo 2 that is
 * 3dfx's `rundll32 3dfxv2ps.dll,UpdateRegSettings`, which the guest tools'
 * guard runs and marks done by writing WINDOWS\V2START.LOG (doc 21 §11). The
 * only wait COMMAND.COM can write is a CHOICE loop, and CHOICE *polls*: the
 * DOS box never idles, the host core it runs on sits at 100 %, and the
 * initialisation it is waiting for is competing with it for the same guest
 * CPU. Measured on `base98-br`, all else equal: the helper takes 1,047 ms at
 * an idle login, 3,625 ms behind a DOS box, and 12,646 ms behind the CHOICE
 * loop. The wait was making the thing it waited for twelve times slower.
 *
 * A Win32 program sleeping costs nothing (the scheduler does not run it),
 * so the wait belongs here rather than in a batch file. With `then=` it
 * also *starts* the batch afterwards, so there is no DOS box open during the
 * wait at all: WIN.INI's `run=` names this, and this names RUN.BAT.
 *
 * Run with no arguments it reads C:\WAITFILE.CFG, because WIN.INI's `run=`
 * takes a program and drops every argument after it:
 *
 *     file=C:\WINDOWS\V2START.LOG
 *     seconds=60
 *     then=C:\RUN.BAT
 *
 * `WAITFILE.EXE <file> [seconds]` waits and exits instead (0 = it appeared,
 * 1 = the time was up), for a batch that wants to wait in the middle.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define CFG_PATH     "C:\\WAITFILE.CFG"
#define POLL_MS      200
#define DEFAULT_SECS 60

/* A Win32 sleep, unlike a DOS box's CHOICE, leaves the guest idle: five
 * wake-ups a second cost nothing measurable and the rest of the guest gets
 * the CPU. Returns 1 if the file turned up, 0 if the time ran out. */
static int wait_for(const char *path, DWORD secs)
{
    DWORD start = GetTickCount();

    for (;;) {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            return 1;
        }
        if (GetTickCount() - start >= secs * 1000) {
            return 0;
        }
        Sleep(POLL_MS);
    }
}

/* `key=value` out of the config file, or 0. Whitespace and CR are trimmed. */
static int cfg_get(const char *key, char *out, size_t len)
{
    FILE *f = fopen(CFG_PATH, "r");
    char line[512];
    size_t klen = strlen(key);
    int got = 0;

    if (!f) {
        return 0;
    }
    while (!got && fgets(line, sizeof line, f)) {
        char *v = line, *e;
        while (*v == ' ' || *v == '\t') v++;
        if (strnicmp(v, key, klen) || v[klen] != '=') {
            continue;
        }
        v += klen + 1;
        e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) {
            *--e = 0;
        }
        if (*v) {
            lstrcpynA(out, v, (int)len);
            got = 1;
        }
    }
    fclose(f);
    return got;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    char file[MAX_PATH] = "", then[MAX_PATH * 2] = "", secs_s[32] = "";
    DWORD secs = DEFAULT_SECS;
    int ok;

    (void)inst; (void)prev; (void)show;

    /* `run=` drops arguments, so a bare start reads the config file. */
    if (args && *args) {
        char *p = args, *q;
        while (*p == ' ' || *p == '\t') p++;
        q = p;
        while (*q && *q != ' ' && *q != '\t') q++;
        if (*q) {
            *q++ = 0;
            while (*q == ' ' || *q == '\t') q++;
            if (*q) {
                secs = (DWORD)atoi(q);
            }
        }
        lstrcpynA(file, p, sizeof file);
    } else {
        if (!cfg_get("file", file, sizeof file)) {
            return 2;               /* nothing asked for: nothing to do */
        }
        if (cfg_get("seconds", secs_s, sizeof secs_s)) {
            secs = (DWORD)atoi(secs_s);
        }
        cfg_get("then", then, sizeof then);
    }
    if (!secs) {
        secs = DEFAULT_SECS;
    }

    ok = wait_for(file, secs);

    /* Start it whether or not the file turned up: a run that waited the
     * whole time should still go ahead, slowly, rather than not at all,
     * and the log this waited for says which happened. */
    if (then[0]) {
        WinExec(then, SW_SHOWNORMAL);
    }
    return ok ? 0 : 1;
}
