/*
 * bsod.c: blue-screen Windows 98 on purpose (doc 19 §29).
 *
 * tools/win98-bsod-test.sh needs a fatal exception on demand. A path that
 * names the CON device twice (`C:\con\con`) faults IFSMGR in every
 * unpatched 98 / 98 SE (Microsoft's fix is Q256015). It has to come from a
 * Win32 process. From a DOS box's `type` it only ends that DOS box with an
 * "illegal operation" dialog, because the VMM terminates a V86 VM that
 * faults and shows the blue screen only for a fault in the Windows VM.
 *
 * The test image is patched (the CreateFile fails and the program lives),
 * so the trigger that matters is ours: bsodvxd.vxd, a dynamic VxD whose
 * init executes an invalid opcode in ring 0, loaded through the
 * `\\.\<path>` door. If this program is still running after both, it says
 * so in a box, so the run's screendumps show why nothing happened.
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

    (void)inst; (void)prev; (void)show;

    /* `BSOD.EXE C:\BSODTMR.VXD`: load the VxD named instead and keep it
     * loaded. bsodtmr.vxd faults from a timer callback a second after its
     * init, and closing the handle (FILE_FLAG_DELETE_ON_CLOSE) would unload
     * it, pending time-out and all, before that. */
    while (cmd && *cmd == ' ') cmd++;
    if (cmd && *cmd) {
        char path[MAX_PATH + 8];

        wsprintfA(path, "\\\\.\\%s", cmd);
        f = CreateFileA(path, 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            Sleep(30000);
            CloseHandle(f);
        }
        MessageBoxA(NULL, "Loading the VxD named on the command line did not "
                    "blue-screen this Windows: is it staged, and does it load?",
                    "bsod", MB_OK | MB_ICONEXCLAMATION);
        return 1;
    }

    /* The con\con bug first. The test image is patched, but an unpatched
     * one reproduces it and it costs nothing. */
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
