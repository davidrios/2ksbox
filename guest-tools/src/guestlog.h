/*
 * guestlog.h: where a guest program's log (and its BMPs) go.
 *
 * A relative name like `fopen("foo.log", "w")` lands in whatever the
 * current directory happens to be: the folder the Run dialog was last in,
 * WINDOWS, a game's own directory, or the read-only CD the EXE was started
 * from, where it is not written at all. So there is one folder, the one
 * SETUP uses for the test programs and SETUP.LOG: C:\2KSBOX. It is ours,
 * on the hard disk, and the same on every machine and every run.
 *
 *   BOXLOG=<dir>   in the environment names another folder (a harness
 *                  putting the log straight onto its scratch disk:
 *                  `set BOXLOG=E:\`)
 *   %TEMP%         the fallback if that folder cannot be made
 *   the current directory   the last resort: a log that cannot be written
 *                  anywhere is worse than one that is hard to find
 *
 * Whichever wins, guest_log_path() is the absolute path it went to, so a
 * console program can end by saying where to read it.
 *
 * Header-only: each of these programs is a single .c file built by its own
 * one-line gcc command, with no object shared between them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GUESTLOG_H
#define GUESTLOG_H

#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <string.h>

/* SETUP puts the TESTS folder and its own SETUP.LOG here
 * (guest-tools/src/setup.c's BOXDIR). */
#define GUEST_DIR "C:\\2KSBOX"

/* A folder plus a name is longer than MAX_PATH when the folder is near the
 * limit, and a silently truncated path writes the wrong file. */
#ifdef _WIN32
#define GUEST_PATHBUF (MAX_PATH * 2)
#else
#define GUEST_PATHBUF 1024
#endif

static char guest_log_file[GUEST_PATHBUF];   /* where the log actually went */

#ifndef _WIN32
/*
 * The same sources build as native host programs too: the reference
 * scene's oracle (tools/d3dgame9-native.cpp and d3dfeat9's, over
 * tools/d3dgame-native/win32_headless.h) is guest-tools/src/d3dgame9.c
 * compiled against DXVK's d3d9. There is no C:\2KSBOX there and no drive
 * letters at all, so a bare name stays in the current directory, which is
 * where that harness runs the oracle and looks for its log.
 */
static __inline const char *guest_path(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "%s", name);
    return buf;
}

static __inline FILE *guest_log_open(const char *name, const char *mode)
{
    FILE *f = fopen(name, mode);

    snprintf(guest_log_file, sizeof guest_log_file, "%s", f ? name : "");
    return f;
}

static __inline const char *guest_log_path(void)
{
    return guest_log_file;
}
#else

/* The output folder, with no trailing backslash, created if it can be. */
static __inline const char *guest_dir(void)
{
    static char dir[MAX_PATH];
    int n;

    if (dir[0]) {
        return dir;
    }
    n = (int)GetEnvironmentVariableA("BOXLOG", dir, sizeof dir);
    if (n <= 0 || n >= (int)sizeof dir) {
        lstrcpynA(dir, GUEST_DIR, sizeof dir);
    }
    /* `set BOXLOG=E:\ & PROG` in a cmd one-liner puts the space before the
     * `&` in the value, and a folder with a trailing space is not the one
     * that was meant: trim both ends before anything is built on it. */
    while (dir[0] == ' ' || dir[0] == '\t') {
        memmove(dir, dir + 1, strlen(dir));
    }
    n = lstrlenA(dir);
    while (n > 0 && (dir[n - 1] == ' ' || dir[n - 1] == '\t')) {
        dir[--n] = '\0';
    }
    if (!dir[0]) {
        lstrcpynA(dir, GUEST_DIR, sizeof dir);
    }
    /* "E:\" and "E:" both mean the root of E:. A root keeps its one
     * backslash and everything else loses a trailing one, so that joining
     * a name below is the same operation either way. */
    n = lstrlenA(dir);
    while (n > 1 && dir[n - 1] == '\\' && !(n == 3 && dir[1] == ':')) {
        dir[--n] = '\0';
    }
    CreateDirectoryA(dir, NULL);   /* already there, or a root: nothing to do */
    return dir;
}

/* <the output folder>\<name>, absolute, in a caller-provided buffer. Use
 * it for anything a program writes for the user to read afterwards, such
 * as the BMPs a test dumps. */
static __inline const char *guest_path(char *buf, size_t n, const char *name)
{
    const char *dir = guest_dir();
    size_t len = strlen(dir);

    _snprintf(buf, n, "%s%s%s", dir, len && dir[len - 1] == '\\' ? "" : "\\", name);
    buf[n - 1] = '\0';
    return buf;
}

/* Open `name` in the output folder, or the best place left. `mode` is
 * fopen's: "w" for a log written once, "a" for one appended to. */
static __inline FILE *guest_log_open(const char *name, const char *mode)
{
    char path[GUEST_PATHBUF];
    FILE *f;

    f = fopen(guest_path(path, sizeof path, name), mode);
    if (!f && GetTempPathA(sizeof path - 16, path)) {
        lstrcatA(path, name);
        f = fopen(path, mode);
    }
    if (!f) {                      /* the current directory, the last resort */
        lstrcpynA(path, name, sizeof path);
        f = fopen(path, mode);
    }
    guest_log_file[0] = '\0';
    if (f && !GetFullPathNameA(path, sizeof guest_log_file, guest_log_file, NULL)) {
        lstrcpynA(guest_log_file, path, sizeof guest_log_file);
    }
    return f;
}

/* Where the last guest_log_open() put its file; "" if none was opened. */
static __inline const char *guest_log_path(void)
{
    return guest_log_file;
}
#endif /* _WIN32 */

#endif /* GUESTLOG_H */
