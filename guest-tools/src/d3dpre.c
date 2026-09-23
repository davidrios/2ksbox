/*
 * d3dpre.c: D3DPRE.EXE, which points this session's DirectDraw at WineD3D when
 * the host has no Direct3D of its own, and at Windows' own when it has
 * (doc 19 §43; Windows 98/Me only).
 *
 * On 9x the WineD3D copy of `DDRAW.DLL` next to a game reaches that game
 * only if it is the first program of the session to touch DirectDraw. Once
 * Windows' own `ddraw.dll` is loaded (and `DDHELP.EXE` keeps it loaded for
 * the rest of the session), every later
 * `LoadLibrary("ddraw.dll")` is answered with that one, whatever sits in the
 * game's folder. So the second game a user starts silently runs on the
 * display driver's DirectDraw, and on a host with no executor that means a
 * game whose 3D setup lists no device at all (doc 19 §42, measured both
 * ways). Keeping a copy loaded from somewhere else does not help: an
 * app-directory module never becomes the machine's, resident or not.
 *
 * What does work is redirecting the *name* before the search happens:
 *
 *   [HKLM\System\CurrentControlSet\Control\SessionManager\KnownDLLs]
 *   "DDRAW"="ddrawme.dll"
 *
 * The loader reads that per `LoadLibrary`, not once at boot, so a value
 * written at login is in force for every program started afterwards, with no
 * restart (measured). Nothing replaces a protected system file,
 * so System File Protection has nothing to restore, and one deleted value
 * puts the machine back.
 *
 * Which way round it should be is the host's business and changes from one
 * run to the next: the same disk image runs on a machine with a Vulkan 1.3
 * card one day and on one without it the next. The adapter says so in
 * `D3D_STATUS`, and the display driver answers a private escape with it
 * (`d3dpt_esc.h`), because a program that is deciding *about* DirectDraw
 * cannot ask DirectDraw. A machine whose display driver is not ours answers
 * nothing, which is its own answer: there is no d3dpt adapter, so there is
 * no executor either, and WineD3D is the only Direct3D on offer.
 *
 * SETUP installs this and the files it points at; it runs from the Run key
 * and exits at once, with nothing to keep resident. Its log stays in
 * WINDOWS rather than going to C:\2KSBOX with every other program's
 * (guestlog.h): it is the marker `WAITFILE.EXE` waits on to know this
 * login's decision has been made, like V2START.LOG. `D3DPRE.LOG` in the
 * Windows folder says what it decided and why.
 *
 * Build: guest-tools/build-wrappers.sh (mingw-w64, i686, msvcrt).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "d3dptvid/d3dpt_esc.h"
#include "../../d3dpt/d3dpt_proto.h"

#define KNOWN_KEY  "System\\CurrentControlSet\\Control\\SessionManager\\KnownDLLs"
#define OURS_KEY   "Software\\2ksbox\\D3dpre"

#define DDRAW_TO    "ddrawme.dll"       /* wine9x's switcher */
#define OPENGL_KEEP "WGLPT32.DLL"       /* our copy of the GL pass-through */

static FILE *logf;

static void logp(const char *fmt, ...)
{
    va_list ap;

    if (!logf) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
}

/* The adapter's answer, through the display driver. Returns 0 when there is
 * no such driver (or no such escape), which the caller reads as "no
 * executor": only our own adapter has one. */
static BOOL host_info(D3DPT_ESC_HOSTINFO_T *hi)
{
    HDC dc = CreateDC("DISPLAY", NULL, NULL, NULL);
    int code = D3DPT_ESC_HOSTINFO;
    BOOL ok = FALSE;

    if (!dc) {
        logp("no display DC");
        return FALSE;
    }
    memset(hi, 0, sizeof(*hi));
    if (ExtEscape(dc, QUERYESCSUPPORT, sizeof(code), (LPCSTR)&code, 0, NULL) > 0 &&
        ExtEscape(dc, D3DPT_ESC_HOSTINFO, 0, NULL, sizeof(*hi), (LPSTR)hi) > 0 &&
        hi->magic == D3DPT_ESC_HOSTINFO_MAGIC) {
        ok = TRUE;
    }
    DeleteDC(dc);
    return ok;
}

static BOOL in_system(const char *name, char *out, size_t n)
{
    UINT len = GetSystemDirectoryA(out, (UINT)n);

    if (!len || len + strlen(name) + 2 > n) {
        return FALSE;
    }
    if (out[len - 1] != '\\') {
        strcat(out, "\\");
    }
    strcat(out, name);
    return GetFileAttributesA(out) != 0xffffffffu;
}

/* Read a KnownDLLs value; TRUE when it is there, with its text in `val`. */
static BOOL known_get(HKEY key, const char *name, char *val, DWORD n)
{
    DWORD type = 0, len = n;

    if (RegQueryValueExA(key, name, NULL, &type, (LPBYTE)val, &len) != ERROR_SUCCESS) {
        return FALSE;
    }
    val[(len < n) ? len : n - 1] = 0;
    return type == REG_SZ;
}

/* Put `name` on `to`, remembering what it said before under our own key so
 * that switching back does not throw away a value that was not ours. */
static void known_set(HKEY known, const char *name, const char *to)
{
    char now[MAX_PATH];
    HKEY ours;

    if (known_get(known, name, now, sizeof(now)) && !lstrcmpiA(now, to)) {
        logp("  %s already -> %s", name, to);
        return;
    }
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, OURS_KEY, 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &ours, NULL) == ERROR_SUCCESS) {
        /* what it said before, so that it can be put back verbatim; an
         * absent value is remembered as the empty string */
        char prev[MAX_PATH];
        DWORD type = 0, len = sizeof(prev);

        if (RegQueryValueExA(ours, name, NULL, &type, (LPBYTE)prev, &len) != ERROR_SUCCESS) {
            if (!known_get(known, name, prev, sizeof(prev))) {
                prev[0] = 0;
            }
            RegSetValueExA(ours, name, 0, REG_SZ, (const BYTE *)prev,
                           (DWORD)strlen(prev) + 1);
        }
        RegCloseKey(ours);
    }
    if (RegSetValueExA(known, name, 0, REG_SZ, (const BYTE *)to,
                       (DWORD)strlen(to) + 1) == ERROR_SUCCESS) {
        logp("  %s -> %s", name, to);
    } else {
        logp("  %s -> %s FAILED", name, to);
    }
}

/* Undo `known_set`: back to whatever was there before us, or gone. Only
 * touches the value while it still says what we put in it. */
static void known_clear(HKEY known, const char *name, const char *to)
{
    char now[MAX_PATH], prev[MAX_PATH];
    HKEY ours;
    DWORD type = 0, len = sizeof(prev);

    if (!known_get(known, name, now, sizeof(now)) || lstrcmpiA(now, to)) {
        return;                         /* not ours to remove */
    }
    prev[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, OURS_KEY, 0, KEY_ALL_ACCESS, &ours) == ERROR_SUCCESS) {
        if (RegQueryValueExA(ours, name, NULL, &type, (LPBYTE)prev, &len) != ERROR_SUCCESS) {
            prev[0] = 0;
        }
        RegDeleteValueA(ours, name);
        RegCloseKey(ours);
    }
    if (prev[0]) {
        RegSetValueExA(known, name, 0, REG_SZ, (const BYTE *)prev, (DWORD)strlen(prev) + 1);
        logp("  %s -> %s (as it was)", name, prev);
    } else {
        RegDeleteValueA(known, name);
        logp("  %s removed", name);
    }
}

/* Is the system OPENGL32.DLL ours? SETUP staged the pass-through over it and
 * kept a second copy as WGLPT32.DLL; if the two differ, Windows has restored
 * its own and WineD3D would be drawing through software GL. Sizes are enough
 * to tell them apart: they are different builds of different things. */
static void gl_restore(void)
{
    char ours[MAX_PATH], sys[MAX_PATH];
    HANDLE a, b;
    DWORD sa = 0, sb = 0;

    if (!in_system(OPENGL_KEEP, ours, sizeof(ours)) ||
        !in_system("OPENGL32.DLL", sys, sizeof(sys))) {
        return;
    }
    a = CreateFileA(ours, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (a != INVALID_HANDLE_VALUE) {
        sa = GetFileSize(a, NULL);
        CloseHandle(a);
    }
    b = CreateFileA(sys, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (b != INVALID_HANDLE_VALUE) {
        sb = GetFileSize(b, NULL);
        CloseHandle(b);
    }
    if (!sa || sa == sb) {
        return;                         /* already ours, or nothing to copy */
    }
    if (CopyFileA(ours, sys, FALSE)) {
        logp("  OPENGL32.DLL was not ours (%lu bytes); the pass-through put back",
             (unsigned long)sb);
    } else {
        logp("  OPENGL32.DLL is not ours (%lu bytes) and is in use, so WineD3D will be slow",
             (unsigned long)sb);
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    D3DPT_ESC_HOSTINFO_T hi;
    char path[MAX_PATH], win[MAX_PATH];
    BOOL asked, want_wine, have_ddraw, have_gl;
    HKEY known;
    UINT n;

    (void)inst; (void)prev; (void)cmd; (void)show;

    n = GetWindowsDirectoryA(win, sizeof(win));
    if (n && n + 16 < sizeof(win)) {
        if (win[n - 1] != '\\') {
            strcat(win, "\\");
        }
        strcat(win, "D3DPRE.LOG");
        logf = fopen(win, "w");
    }

    asked = host_info(&hi);
    /* No escape means no adapter of ours, and our Direct3D lives in that
     * adapter: WineD3D is then the only one there is. */
    want_wine = !asked || hi.d3d_status != D3DPT_STATUS_READY;
    logp("d3dpre: adapter %s, d3d status %lu -> DirectDraw should be %s",
         asked ? "answered" : "did not answer (not ours)",
         asked ? (unsigned long)hi.d3d_status : 0ul,
         want_wine ? "WineD3D's" : "Windows' own");

    have_ddraw = in_system(DDRAW_TO, path, sizeof(path));
    have_gl = in_system(OPENGL_KEEP, path, sizeof(path));
    if (want_wine && !have_ddraw) {
        /* Redirecting a name at a file that is not there would take
         * DirectDraw away from every program on the machine. */
        logp("d3dpre: %s is not in the system folder, so there is nothing to switch to",
             DDRAW_TO);
        want_wine = FALSE;
    }

    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, KNOWN_KEY, 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &known, NULL) != ERROR_SUCCESS) {
        logp("d3dpre: cannot open KnownDLLs");
        if (logf) fclose(logf);
        return 1;
    }
    if (want_wine) {
        known_set(known, "DDRAW", DDRAW_TO);
        /* WineD3D draws through the first opengl32.dll the loader finds, and
         * Microsoft's is software GL. SETUP puts the pass-through in as the
         * system one; if Windows has restored its own since (System File
         * Protection does that to files it knows), put ours back. Nothing
         * has loaded GL this early in a login. */
        if (have_gl) {
            gl_restore();
        }
    } else {
        known_clear(known, "DDRAW", DDRAW_TO);
        known_clear(known, "OPENGL32", "wglpt32.dll");   /* an earlier build set this */
    }
    RegCloseKey(known);
    logp("d3dpre: done (this session, and every program started after it)");
    if (logf) {
        fclose(logf);
    }
    return 0;
}
