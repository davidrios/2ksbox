/*
 * The qmake cxx-qt's build asks, on a native Windows build in MSYS2
 * (scripts/build-windows.sh, docs/build-windows.md "Building on Windows").
 *
 * qt-build-utils runs every Qt tool it finds with an empty environment, to
 * keep a stray library path from mixing Qt versions. MSYS2 installs moc,
 * rcc, qmltyperegistrar and qmlcachegen in share/qt6/bin, away from the
 * DLLs they load in bin/, so with no PATH none of them can start: "moc
 * unexpectedly exited". build-windows.sh copies those tools next to their
 * DLLs in build/win/qt-host, and this program is the QMAKE that points
 * there: the tool-directory queries are answered with that folder, and
 * everything else goes to MSYS2's own qmake6 unchanged.
 *
 * REAL_QMAKE and HOST_TOOLS come from qt-host-paths.h, written beside it by
 * build-windows.sh. Linked -static: it has to start with no PATH as well.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "qt-host-paths.h"

/* Windows command-line quoting: what CommandLineToArgvW splits back. */
static size_t put(char *cmd, size_t n, size_t cap, char c)
{
    if (n + 1 < cap) {
        cmd[n++] = c;
    }
    return n;
}

static void append_arg(char *cmd, size_t cap, const char *arg)
{
    size_t n = strlen(cmd), bs, i;
    const char *p;

    if (n) {
        n = put(cmd, n, cap, ' ');
    }
    if (*arg && !strpbrk(arg, " \t\"")) {
        for (p = arg; *p; p++) {
            n = put(cmd, n, cap, *p);
        }
        cmd[n] = '\0';
        return;
    }
    n = put(cmd, n, cap, '"');
    for (p = arg;; p++) {
        for (bs = 0; *p == '\\'; p++) {
            bs++;
        }
        if (*p == '\0') {
            /* doubled, or the closing quote would be escaped */
            for (i = 0; i < bs * 2; i++) {
                n = put(cmd, n, cap, '\\');
            }
            break;
        }
        if (*p == '"') {
            for (i = 0; i < bs * 2 + 1; i++) {
                n = put(cmd, n, cap, '\\');
            }
        } else {
            for (i = 0; i < bs; i++) {
                n = put(cmd, n, cap, '\\');
            }
        }
        n = put(cmd, n, cap, *p);
    }
    n = put(cmd, n, cap, '"');
    cmd[n] = '\0';
}

int main(int argc, char **argv)
{
    static const char *const tool_dirs[] = {
        "QT_HOST_LIBEXECS",    "QT_HOST_LIBEXECS/get",
        "QT_INSTALL_LIBEXECS", "QT_INSTALL_LIBEXECS/get",
    };
    static char cmd[32768];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code;
    size_t i;
    int a;

    if (argc == 3 && strcmp(argv[1], "-query") == 0) {
        for (i = 0; i < sizeof(tool_dirs) / sizeof(tool_dirs[0]); i++) {
            if (strcmp(argv[2], tool_dirs[i]) == 0) {
                printf("%s\n", HOST_TOOLS);
                return 0;
            }
        }
    }

    append_arg(cmd, sizeof(cmd), REAL_QMAKE);
    for (a = 1; a < argc; a++) {
        append_arg(cmd, sizeof(cmd), argv[a]);
    }
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    fflush(stdout);
    if (!CreateProcessA(REAL_QMAKE, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "qmake-host: cannot run %s (error %lu)\n", REAL_QMAKE,
                (unsigned long)GetLastError());
        return 127;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
