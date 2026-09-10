/*
 * bsod.c — blue-screen Windows 98 on purpose (doc 19 §29).
 *
 * The test tools/win98-bsod-test.sh needs a fatal exception it can cause on
 * demand, and Windows 98 obliges: a path that names the CON device twice
 * (`C:\con\con`) sends IFSMGR into a fault in every unpatched 98 / 98 SE
 * (Microsoft's Q256015 is the fix, and a 2ksbox image is a period install).
 * It has to come from a **Win32 process**: the same path from a DOS box's
 * `type` only ends that DOS box with an "illegal operation" dialog — the
 * VMM terminates a V86 VM that faults, and shows the blue screen only for
 * a fault in the Windows VM.
 *
 * The test image turned out to be patched (2026-09-09: the CreateFile came
 * back with an error and the program lived), so the trigger that matters is
 * ours: bsodvxd.vxd, a dynamic VxD whose init executes an invalid opcode in
 * ring 0, loaded through the `\\.\<path>` door. If this program is still
 * running after both, say so in a box rather than exit silently, so the
 * run's screendumps show why nothing happened.
 *
 * Build: guest-tools/build-driver9x.sh (i686-w64-mingw32, no CRT needed
 * beyond kernel32/user32).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    HANDLE f;

    (void)inst; (void)prev; (void)cmd; (void)show;
    /* the famous one first — patched on the test image, kept because an
     * unpatched one reproduces it and it costs nothing */
    f = CreateFileA("C:\\con\\con", GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);

    /* ours: a dynamic VxD whose init executes ud2 in ring 0 (bsodvxd.c).
     * Staged beside this program on C:\ by the test. */
    f = CreateFileA("\\\\.\\C:\\BSODVXD.VXD", 0, 0, NULL, 0,
                    FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);

    MessageBoxA(NULL, "Neither C:\\con\\con nor loading C:\\BSODVXD.VXD "
                "blue-screened this Windows: is the VxD staged on C:\\?",
                "bsod", MB_OK | MB_ICONEXCLAMATION);
    return 1;
}
