/*
 * v2start.c: V2START.EXE, the Voodoo 2's start-up guard (doc 21 §11).
 *
 * 3dfx's Voodoo 2 driver puts a Run entry, `Voodoo2`, in the registry:
 * `rundll32.exe 3dfxv2ps.dll,UpdateRegSettings` at every login. That call
 * brings the card up through Glide 3's init library in its own process
 * (video registers zeroed, command FIFO off), and under TCG it takes about
 * three seconds. A Glide game started inside them has the card torn down
 * under its open window and hangs. On real hardware it is milliseconds,
 * so nothing in 3dfx's driver guards it.
 *
 * SETUP.EXE moves that command out of the Run key into ours
 * (HKLM\SOFTWARE\2ksbox\Voodoo2, `Command`) and puts this program in its
 * place. At login this runs the command itself, waits for it to finish,
 * and while it runs keeps a small window up saying so, on top of the
 * desktop, so whoever is at the machine knows to hold off a 3dfx game.
 * It only informs: the desktop stays usable, and closing the window hides
 * it without ending the wait. It appears only if the init is still
 * running after half a second, so a fast host sees nothing.
 *
 * If the Run entry is back (3dfx's driver was reinstalled since SETUP),
 * Explorer has already started that copy this login: it is moved again
 * for next time, and this login waits for the rundll32 already running
 * rather than starting a second init on top of it.
 *
 * Either way the wait is bounded (V2_TIMEOUT_MS): a wedged init must not
 * leave the notice up for ever. What happened goes to
 * %windir%\V2START.LOG, one line per login, rewritten each time.
 *
 * Windows 98 / Me only: 3dfx's 2000/XP driver has no such entry.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

#define RUN_KEY     "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_3DFX    "Voodoo2"
#define OUR_KEY     "SOFTWARE\\2ksbox\\Voodoo2"
#define OUR_VALUE   "Command"
#define V2_SHOW_MS    500
#define V2_TIMEOUT_MS 120000
#define V2_FIND_MS    5000

#define NOTICE_W 400
#define NOTICE_H 110

static HWND g_notice;
static const char g_msg[] = "Voodoo 2 driver is loading, please wait before running 3dfx games.";

static int reg_get(HKEY root, const char *key, const char *name, char *out, DWORD len)
{
    HKEY k;
    DWORD type, n = len;
    LONG rc;

    if (RegOpenKeyExA(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    rc = RegQueryValueExA(k, name, NULL, &type, (BYTE *)out, &n);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_SZ || n == 0)
        return 0;
    out[len - 1] = 0;
    return out[0] != 0;
}

/* 3dfx's entry, if it is in the Run key again: kept as our command and
 * taken out of the Run key. Returns whether it was there. */
static int take_run_entry(char *cmd, DWORD len)
{
    HKEY k;

    if (!reg_get(HKEY_LOCAL_MACHINE, RUN_KEY, RUN_3DFX, cmd, len))
        return 0;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, OUR_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, OUR_VALUE, 0, REG_SZ, (const BYTE *)cmd, (DWORD)strlen(cmd) + 1);
        RegCloseKey(k);
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, RUN_KEY, 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        RegDeleteValueA(k, RUN_3DFX);
        RegCloseKey(k);
    }
    return 1;
}

/* The DLL a `rundll32 <dll>,<entry>` command names is on the search path:
 * a machine whose card (and driver) is gone must not get rundll32's
 * "Error loading" box at every login. */
static int command_runnable(const char *cmd)
{
    char dll[MAX_PATH], found[MAX_PATH], *comma, *space;
    const char *p = cmd;

    space = strchr(p, ' ');
    if (!space)
        return 1;
    p = space + 1;
    while (*p == ' ')
        p++;
    lstrcpynA(dll, p, sizeof dll);
    comma = strchr(dll, ',');
    if (!comma)
        return 1;
    *comma = 0;
    return SearchPathA(NULL, dll, NULL, sizeof found, found, NULL) != 0;
}

/* A RUNDLL32 process already running: the copy Explorer started from a
 * Run entry that came back. Toolhelp is there on 98 and Me. */
static HANDLE find_rundll32(void)
{
    PROCESSENTRY32 pe;
    HANDLE snap, h = NULL;
    const char *base;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return NULL;
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32First(snap, &pe); ok && !h; ok = Process32Next(snap, &pe)) {
        base = strrchr(pe.szExeFile, '\\');
        base = base ? base + 1 : pe.szExeFile;
        if (lstrcmpiA(base, "RUNDLL32.EXE") == 0)
            h = OpenProcess(SYNCHRONIZE, FALSE, pe.th32ProcessID);
    }
    CloseHandle(snap);
    return h;
}

static LRESULT CALLBACK notice_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT r, t;
        HDC dc = BeginPaint(w, &ps);

        GetClientRect(w, &r);
        DrawIcon(dc, 16, (r.bottom - GetSystemMetrics(SM_CYICON)) / 2, LoadIcon(NULL, IDI_INFORMATION));
        r.left = 16 + GetSystemMetrics(SM_CXICON) + 12;
        r.right -= 16;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        /* wrapped text has no DT_VCENTER: measure it, then centre it */
        t = r;
        DrawTextA(dc, g_msg, -1, &t, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
        r.top = (r.bottom - (t.bottom - t.top)) / 2;
        DrawTextA(dc, g_msg, -1, &r, DT_LEFT | DT_WORDBREAK);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_TIMER:
        KillTimer(w, 1);
        SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        return 0;
    case WM_CLOSE:          /* hide it; the wait goes on */
        ShowWindow(w, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static void make_notice(HINSTANCE inst)
{
    WNDCLASSA wc;
    RECT r = { 0, 0, NOTICE_W, NOTICE_H };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;
    int ww, wh;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = notice_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "2ksboxV2Start";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);
    AdjustWindowRectEx(&r, style, FALSE, ex);
    ww = r.right - r.left;
    wh = r.bottom - r.top;
    g_notice = CreateWindowExA(ex, wc.lpszClassName, "Voodoo 2", style,
                               (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                               ww, wh, NULL, NULL, inst, NULL);
    if (g_notice)
        SetTimer(g_notice, 1, V2_SHOW_MS, NULL);
}

/* Pump the notice's messages until `h` is signalled or the time is up.
 * Returns the milliseconds waited, or -1 on the timeout. */
static long wait_pumping(HANDLE h, DWORD start)
{
    MSG msg;

    for (;;) {
        DWORD now = GetTickCount() - start, left, rc;

        if (now >= V2_TIMEOUT_MS)
            return -1;
        left = V2_TIMEOUT_MS - now;
        rc = MsgWaitForMultipleObjects(1, &h, FALSE, left, QS_ALLINPUT);
        if (rc == WAIT_OBJECT_0)
            return (long)(GetTickCount() - start);
        if (rc != WAIT_OBJECT_0 + 1)
            return -1;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    char cmd[MAX_PATH * 2] = "", logpath[MAX_PATH], what[64] = "";
    HANDLE h = NULL;
    DWORD start = GetTickCount(), code = 0;
    long waited = 0;
    int again, have_cmd = 1, waited_on = 0;
    FILE *log;

    (void)prev; (void)args; (void)show;
    again = take_run_entry(cmd, sizeof cmd);
    if (!again && !reg_get(HKEY_LOCAL_MACHINE, OUR_KEY, OUR_VALUE, cmd, sizeof cmd)) {
        have_cmd = 0;
        lstrcpyA(what, "no 3dfx start-up command: nothing to wait for");
    } else if (!command_runnable(cmd)) {
        lstrcpyA(what, "the command's DLL is not on this machine: not run");
    } else if (again) {
        /* Explorer started this one too, a moment before us. Any
         * RUNDLL32 will do: the other login-time one (the power scheme)
         * exits at once, and waiting on it only costs its lifetime. */
        DWORD t0 = GetTickCount();
        while (!(h = find_rundll32()) && GetTickCount() - t0 < V2_FIND_MS)
            Sleep(100);
        lstrcpyA(what, h ? "the Run entry was back: waited for Explorer's rundll32"
                         : "the Run entry was back: no rundll32 found to wait for");
    } else {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;

        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            h = pi.hProcess;
            lstrcpyA(what, "ran");
        } else {
            wsprintfA(what, "CreateProcess failed (%lu)", GetLastError());
        }
    }
    if (h) {
        waited_on = 1;
        make_notice(inst);
        waited = wait_pumping(h, start);
        if (waited >= 0 && !again)
            GetExitCodeProcess(h, &code);
        CloseHandle(h);
        if (g_notice)
            DestroyWindow(g_notice);
    }

    GetWindowsDirectoryA(logpath, sizeof logpath);
    lstrcatA(logpath, "\\V2START.LOG");
    if ((log = fopen(logpath, "w"))) {       /* text mode: \n is written CRLF */
        fprintf(log, "v2start: %s\n", what);
        fprintf(log, "v2start: command: %s\n", have_cmd ? cmd : "-");
        if (waited_on && waited >= 0)
            fprintf(log, "v2start: done in %ld ms, exit code %lu\n", waited, code);
        else if (waited_on)
            fprintf(log, "v2start: gave up after %d ms, still running\n", V2_TIMEOUT_MS);
        fclose(log);
    }
    return 0;
}
