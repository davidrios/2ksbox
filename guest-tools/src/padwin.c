/*
 * padwin.c — the USB HID gamepad as a Windows game finds it (M13 path A,
 * docs/tracks/m13-gamepads.md).
 *
 *   PADWIN [samples]      XP, 2000, 98 SE / Me. Default 200 samples,
 *                         ~4 a second.
 *
 * Named PADWIN and not PADTEST because `PADTEST.COM` — the DOS gameport
 * probe — is in the same folder of the guest-tools ISO, and both DOS and
 * cmd resolve a bare name to the `.COM` first. Typing PADTEST on XP would
 * run a DOS program that reads port 0x201 under NTVDM and report nothing
 * about the device this file is for.
 *
 * What it is for: path A was confirmed by hand (a real controller in the
 * Game Controllers panel on XP and 98 SE) and nothing re-checked it after
 * a change. This is that check, and it asks the questions a game asks
 * rather than what the control panel shows.
 *
 * Two of them, because a title of the era can call either API and the pad
 * has to arrive through both. **DirectInput** is the one a 1998-and-later
 * game uses; **winmm** — `joyGetDevCaps` / `joyGetPosEx`, the multimedia
 * joystick API on top of 9x's VJOYD — is what a great many mid-90s
 * Windows titles call, and it is the one that decides whether Windows 98
 * needs the gameport's driver half at all (M13 step 7): if a USB HID pad
 * reaches winmm there, a Windows game on 98 already has a joystick and
 * nobody has to install "Standard Game Port" for one. It does, which is
 * why that step was dropped — so this is the check that claim rests on,
 * and both halves are read every sample rather than counted once.
 *
 * The DirectInput half:
 *
 *   * enumerate attached joysticks and name them, because "no gamepad"
 *     and "a gamepad with no axes" are different failures and the
 *     descriptor bytes decide which one a wrong build produces;
 *   * put every axis on a 0..255 range, which is the *same* range the
 *     report carries, so a printed X is the byte gamepad::hid_axis() made
 *     and the numbers can be compared to what the host sent;
 *   * read the POV hat, which is where a missing null state shows up — a
 *     released hat reads as north rather than as centred;
 *   * read the buttons, in the order gamepad::HID_BUTTONS fixes.
 *
 * The winmm half puts its axes on the same 0..255 range, rescaled from
 * whatever `JOYCAPS` says the driver reports, so the two columns can be
 * read against each other sample by sample: `WX` beside `X` is the same
 * pad seen through the other API. Its POV stays in winmm's own units
 * (hundredths of a degree, 65535 centred) rather than being folded into
 * DirectInput's -1, because a value nothing here invented is the one
 * worth having in a log.
 *
 * Output goes three ways, because the harness, a person at the machine
 * and a post-mortem all want it: COM1 (opened directly, so the Run dialog
 * can start this with no shell to redirect), stdout, and %TEMP%\padwin.log
 * — the ISO it runs from is read-only, so the log cannot live beside it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define DIRECTINPUT_VERSION 0x0700
#include <windows.h>
#include <dinput.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *logf;
static HANDLE com1 = INVALID_HANDLE_VALUE;

/* The first joystick winmm answers `joyGetDevCaps` for, and its ranges:
 * the axes come back on whatever range the driver chose, so the caps are
 * needed to put them on the report's own 0..255 beside the DirectInput
 * column. -1 until something enumerates. */
static int winmm_id = -1;
static JOYCAPSA winmm_caps;

static void say(const char *fmt, ...)
{
    char buf[512];
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = (int)strlen(buf);
    }
    buf[n] = 0;
    fputs(buf, stdout);
    fflush(stdout);
    if (logf) {
        fputs(buf, logf);
        fflush(logf);
    }
    if (com1 != INVALID_HANDLE_VALUE) {
        DWORD wrote;
        WriteFile(com1, buf, (DWORD)n, &wrote, NULL);
    }
}

/*
 * COM1 as a plain file handle. No shell is involved in starting this —
 * the harness types it into the Run dialog — so the program has to reach
 * the serial line itself. A machine without one just logs to the file.
 */
static void open_com1(void)
{
    DCB dcb;

    com1 = CreateFileA("COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (com1 == INVALID_HANDLE_VALUE) {
        return;
    }
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (GetCommState(com1, &dcb)) {
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        SetCommState(com1, &dcb);
    }
}

/* --- the window DirectInput wants ---------------------------------- */

/*
 * A real window, because SetCooperativeLevel takes one and a console
 * application has no HWND on Windows 98 (GetConsoleWindow is 2000 and
 * later). Small, visible and pumped: a screendump of a failing run then
 * shows something rather than a blank desktop.
 */
static HWND make_window(void)
{
    WNDCLASSA wc;
    HWND hwnd;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "padwin";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "padwin", "padwin — gamepad probe",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 380, 140,
                           NULL, NULL, wc.hInstance, NULL);
    return hwnd;
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* --- DirectInput ---------------------------------------------------- */

static LPDIRECTINPUTA di;
static LPDIRECTINPUTDEVICE2A joy;
static int n_found;

static BOOL CALLBACK enum_joy(LPCDIDEVICEINSTANCEA inst, LPVOID ctx)
{
    LPDIRECTINPUTDEVICEA dev = NULL;

    n_found++;
    say("device %d: \"%s\" (%s)\n", n_found, inst->tszInstanceName,
        inst->tszProductName);
    if (joy) {
        return DIENUM_CONTINUE;           /* name them all, drive the first */
    }
    if (FAILED(IDirectInput_CreateDevice(di, &inst->guidInstance, &dev, NULL))) {
        say("  CreateDevice failed\n");
        return DIENUM_CONTINUE;
    }
    /*
     * IDirectInputDevice2 for Poll(). A HID gamepad is polled rather than
     * event-driven, and without the Poll the immediate state on 9x is
     * whatever the last event left there.
     */
    if (FAILED(IDirectInputDevice_QueryInterface(dev, &IID_IDirectInputDevice2A,
                                                 (void **)&joy))) {
        say("  no IDirectInputDevice2 (DirectX too old?)\n");
        joy = NULL;
    }
    IDirectInputDevice_Release(dev);
    (void)ctx;
    return DIENUM_CONTINUE;
}

/*
 * Every axis on 0..255, which is what the report itself carries — so a
 * number printed below is the byte the host put in the report, and the
 * two ends of the range are 0x00 and 0xff exactly. DirectInput's own
 * default is device-dependent and would make every reading a fraction of
 * an unknown.
 */
static BOOL CALLBACK set_range(LPCDIDEVICEOBJECTINSTANCEA obj, LPVOID ctx)
{
    DIPROPRANGE r;

    memset(&r, 0, sizeof r);
    r.diph.dwSize = sizeof r;
    r.diph.dwHeaderSize = sizeof r.diph;
    r.diph.dwHow = DIPH_BYID;
    r.diph.dwObj = obj->dwType;
    r.lMin = 0;
    r.lMax = 255;
    IDirectInputDevice2_SetProperty((LPDIRECTINPUTDEVICE2A)ctx, DIPROP_RANGE,
                                    &r.diph);
    return DIENUM_CONTINUE;
}

/* The winmm view, which is what a lot of era titles actually used. One
 * line, as a cross-check on the DirectInput one: a pad that enumerates in
 * one and not the other is a fact worth having in the log. */
static void report_winmm(void)
{
    JOYCAPSA caps;
    JOYINFOEX info;
    UINT n = joyGetNumDevs();
    UINT attached = 0;
    UINT i;

    /* joyGetNumDevs() is how many the driver *supports*, not how many are
     * there — 16 on a machine with none plugged in — so the count that
     * means anything is of the ones that answer joyGetDevCaps. Said
     * plainly either way: a run where nothing enumerates used to leave no
     * winmm line at all, which reads like the probe skipping the check. */
    for (i = 0; i < n && i < 16; i++) {
        if (joyGetDevCapsA(i, &caps, sizeof caps) != JOYERR_NOERROR) {
            continue;
        }
        attached++;
        if (winmm_id < 0) {
            winmm_id = (int)i;
            winmm_caps = caps;
        }
        memset(&info, 0, sizeof info);
        info.dwSize = sizeof info;
        info.dwFlags = JOY_RETURNALL;
        say("winmm: joystick %u \"%s\" %u axes %u buttons%s\n", i,
            caps.szPname, (unsigned)caps.wNumAxes, (unsigned)caps.wNumButtons,
            joyGetPosEx(i, &info) == JOYERR_NOERROR ? " present" : " not attached");
    }
    say("winmm: %u supported, %u attached\n", (unsigned)n, (unsigned)attached);
}

/* One axis of winmm's reading, on 0..255. The driver's own range is what
 * JOYCAPS says it is — 0..65535 on the HID mapper here, but nothing
 * guarantees that — so it is rescaled rather than assumed, and an axis
 * whose caps are degenerate reads -1 instead of dividing by zero. */
static long wnorm(DWORD pos, DWORD lo, DWORD hi)
{
    if (hi <= lo) {
        return -1;
    }
    if (pos < lo) {
        pos = lo;
    }
    if (pos > hi) {
        pos = hi;
    }
    return (long)(((pos - lo) * 255 + (hi - lo) / 2) / (hi - lo));
}

/* The same pad through the multimedia API. Every field reads -1 when
 * winmm has no joystick at all, which is a different fact from a joystick
 * that reads centred and the check has to be able to tell them apart. */
static void read_winmm(long *ax, long *pov, long *btn)
{
    JOYINFOEX info;
    int k;

    for (k = 0; k < 4; k++) {
        ax[k] = -1;
    }
    *pov = -1;
    *btn = -1;
    if (winmm_id < 0) {
        return;
    }
    memset(&info, 0, sizeof info);
    info.dwSize = sizeof info;
    info.dwFlags = JOY_RETURNALL;
    if (joyGetPosEx((UINT)winmm_id, &info) != JOYERR_NOERROR) {
        return;
    }
    ax[0] = wnorm(info.dwXpos, winmm_caps.wXmin, winmm_caps.wXmax);
    ax[1] = wnorm(info.dwYpos, winmm_caps.wYmin, winmm_caps.wYmax);
    ax[2] = wnorm(info.dwZpos, winmm_caps.wZmin, winmm_caps.wZmax);
    ax[3] = wnorm(info.dwRpos, winmm_caps.wRmin, winmm_caps.wRmax);
    *pov = (long)info.dwPOV;
    *btn = (long)info.dwButtons;
}

int main(int argc, char **argv)
{
    int samples = argc > 1 ? atoi(argv[1]) : 200;
    char path[MAX_PATH];
    HWND hwnd;
    HRESULT hr;
    DWORD ver;
    int i;

    open_com1();
    if (GetTempPathA(sizeof path, path)) {
        strncat(path, "padwin.log", sizeof path - strlen(path) - 1);
        logf = fopen(path, "w");
    }
    say("padwin: DirectInput gamepad probe\n");

    hwnd = make_window();
    pump();

    /*
     * Newest first, then older ones. dinput.dll refuses a version it
     * predates (DIERR_OLDDIRECTINPUTVERSION), and a Windows 98 that never
     * had a DirectX update past its own is exactly the machine this has
     * to keep working on. Only the base methods are used afterwards, so
     * an older interface is no loss here.
     */
    for (ver = 0x0700; ; ver = ver == 0x0700 ? 0x0500 : 0x0300) {
        hr = DirectInputCreateA(GetModuleHandleA(NULL), ver, &di, NULL);
        if (SUCCEEDED(hr)) {
            say("DirectInput version 0x%04x\n", (unsigned)ver);
            break;
        }
        say("DirectInputCreate(0x%04x) -> 0x%08lx\n", (unsigned)ver, (long)hr);
        if (ver == 0x0300) {
            say("FAIL no DirectInput at all\n");
            return 2;
        }
    }

    IDirectInput_EnumDevices(di, DIDEVTYPE_JOYSTICK, enum_joy, NULL,
                             DIEDFL_ATTACHEDONLY);
    say("devices: %d\n", n_found);
    report_winmm();
    if (!joy) {
        /* The one failure worth spelling out: on 98 the New Hardware
         * wizard wants the Windows source files the first time a HID pad
         * is plugged in, and until someone answers it there is no
         * joystick to find. */
        say("FAIL no joystick enumerated (is the pad's driver installed?)\n");
        say("DONE\n");
        return 1;
    }

    if (FAILED(hr = IDirectInputDevice2_SetDataFormat(joy, &c_dfDIJoystick))) {
        say("FAIL SetDataFormat 0x%08lx\n", (long)hr);
        say("DONE\n");
        return 1;
    }
    /* Background and non-exclusive: the harness drives this through the
     * Run dialog and nothing guarantees the window is in front. A game
     * would take the foreground; that is not what is under test here. */
    if (FAILED(hr = IDirectInputDevice2_SetCooperativeLevel(
                   joy, hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        say("FAIL SetCooperativeLevel 0x%08lx\n", (long)hr);
        say("DONE\n");
        return 1;
    }
    IDirectInputDevice2_EnumObjects(joy, set_range, joy, DIDFT_AXIS);
    if (FAILED(hr = IDirectInputDevice2_Acquire(joy))) {
        say("FAIL Acquire 0x%08lx\n", (long)hr);
        say("DONE\n");
        return 1;
    }
    say("acquired, %d samples\n", samples);

    for (i = 0; i < samples; i++) {
        DIJOYSTATE js;
        long w[4];
        long wpov, wb;
        char wbs[16];
        unsigned b = 0;
        int k;

        pump();
        IDirectInputDevice2_Poll(joy);
        hr = IDirectInputDevice2_GetDeviceState(joy, sizeof js, &js);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            IDirectInputDevice2_Acquire(joy);
            Sleep(50);
            continue;
        }
        if (FAILED(hr)) {
            say("GetDeviceState 0x%08lx\n", (long)hr);
            Sleep(250);
            continue;
        }
        for (k = 0; k < 12; k++) {
            if (js.rgbButtons[k] & 0x80) {
                b |= 1u << k;
            }
        }
        /* POV as DirectInput reports it: hundredths of a degree clockwise
         * from north, or -1 (0xffffffff) centred. The centred value is
         * the null state in the report descriptor; without it this reads
         * 0 — north — with nothing pressed. */
        /* Read winmm immediately after, so the two columns are as close
         * to the same instant as this can make them. */
        read_winmm(w, &wpov, &wb);
        /* A button mask is hex like the DirectInput one, but "no winmm
         * joystick" has to stay -1 rather than becoming ffffffff — the
         * two are different findings and the check reads this field. */
        if (wb < 0) {
            strcpy(wbs, "-1");
        } else {
            sprintf(wbs, "%03lx", wb);
        }
        say("X=%ld Y=%ld Z=%ld Rz=%ld POV=%ld B=%03x"
            " WX=%ld WY=%ld WZ=%ld WR=%ld WPOV=%ld WB=%s\n",
            js.lX, js.lY, js.lZ, js.lRz, (long)(int)js.rgdwPOV[0], b,
            w[0], w[1], w[2], w[3], wpov, wbs);
        Sleep(250);
    }

    IDirectInputDevice2_Unacquire(joy);
    IDirectInputDevice2_Release(joy);
    IDirectInput_Release(di);
    say("DONE\n");
    return 0;
}
