/*
 * win32_headless.h: the two dozen Win32 calls guest-tools/src/d3dgame.h makes,
 * with no windowing toolkit at all, so the reference scene (doc 14 P0a)
 * compiles unmodified as a native program against DXVK's d3d9 (the host
 * executor, ADR-007) and its -dump BMPs diff against the rig goldens
 * (reference/d3d). Include before the scene source; C++ only (DXVK's native
 * COM headers have no C mode).
 *
 * There is no window: CreateWindowA returns NULL, which is what DXVK's
 * headless WSI (patches/dxvk/04-wsi-headless, DXVK_WSI_DRIVER=Headless)
 * expects. With a NULL device window d3d9 creates no presenter and Present
 * is a no-op, so the scene renders entirely off-screen and the frame comes
 * out through GetRenderTargetData — the same path the paravirtual device's
 * executor takes. That means the oracle runs on a machine with no display,
 * and it is why nothing here needs SDL any more (2026-09-07; the SDL shim
 * this replaced also owned the last libSDL2 dependency in the tree).
 *
 * The trade: no interactive run. The message pump is always empty and no key
 * is ever down, so drive the scene with -frames / -dump, never by hand.
 */
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

typedef intptr_t LRESULT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef HANDLE HCURSOR;
typedef HANDLE HICON;
typedef HANDLE HBRUSH;
typedef HANDLE HMENU;
#define CALLBACK
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef struct tagWNDCLASSA {
  UINT style; WNDPROC lpfnWndProc; int cbClsExtra; int cbWndExtra; HINSTANCE hInstance;
  HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName; LPCSTR lpszClassName;
} WNDCLASSA;
typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct _OSVERSIONINFOA { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; char szCSDVersion[128]; } OSVERSIONINFOA;

enum { WM_DESTROY = 0x0002, WM_QUIT = 0x0012, WM_SETCURSOR = 0x0020, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101 };
enum { VK_SPACE = 0x20, VK_ESCAPE = 0x1B, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_F1 = 0x70 };
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define WS_POPUP            0x80000000L
#define WS_THICKFRAME       0x00040000L
#define WS_MAXIMIZEBOX      0x00010000L
#define CW_USEDEFAULT       ((int)0x80000000)
#define SW_SHOW 5
#define PM_REMOVE 1
#define IDC_ARROW 32512
#define GWLP_USERDATA (-21)

namespace win32hl {
  static WNDPROC g_wndproc;
  static LONG_PTR g_userdata;
}

inline LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) { return 0; }
inline LONG_PTR GetWindowLongPtrA(HWND, int) { return win32hl::g_userdata; }
inline LONG_PTR SetWindowLongPtrA(HWND, int, LONG_PTR v) { LONG_PTR o = win32hl::g_userdata; win32hl::g_userdata = v; return o; }
inline HMODULE GetModuleHandleA(LPCSTR) { return nullptr; }
inline HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return nullptr; }
inline HCURSOR SetCursor(HCURSOR) { return nullptr; }
inline BOOL AdjustWindowRect(RECT *, DWORD, BOOL) { return TRUE; }
inline BOOL ShowWindow(HWND, int) { return TRUE; }
inline BOOL SetForegroundWindow(HWND) { return TRUE; }
inline BOOL TranslateMessage(const MSG *) { return FALSE; }
inline LRESULT DispatchMessageA(const MSG *m) { return win32hl::g_wndproc ? win32hl::g_wndproc(m->hwnd, m->message, m->wParam, m->lParam) : 0; }
inline WORD RegisterClassA(const WNDCLASSA *wc) { win32hl::g_wndproc = wc->lpfnWndProc; return 1; }
inline BOOL SetWindowTextA(HWND, LPCSTR) { return TRUE; }   /* no title bar to write to */
inline short GetAsyncKeyState(int) { return 0; }            /* no keyboard: -frames ends the run */
inline HMODULE LoadLibraryA(LPCSTR n) { return dlopen(n, RTLD_NOW); }
inline void *GetProcAddress(HMODULE m, LPCSTR n) { return m ? dlsym(m, n) : nullptr; }

inline DWORD GetTickCount(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (DWORD)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}
inline void Sleep(DWORD ms) {
  struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, nullptr);
}

/* NULL window: DXVK's headless WSI, no presenter, Present is a no-op */
inline HWND CreateWindowA(LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, void *) {
  return nullptr;
}

inline BOOL PeekMessageA(MSG *, HWND, UINT, UINT, UINT) { return FALSE; }

inline void GetLocalTime(SYSTEMTIME *st) {
  time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt);
  st->wYear = lt.tm_year + 1900; st->wMonth = lt.tm_mon + 1; st->wDay = lt.tm_mday; st->wDayOfWeek = lt.tm_wday;
  st->wHour = lt.tm_hour; st->wMinute = lt.tm_min; st->wSecond = lt.tm_sec; st->wMilliseconds = 0;
}
inline BOOL GetVersionExA(OSVERSIONINFOA *v) {
  v->dwMajorVersion = 0; v->dwMinorVersion = 0; v->dwBuildNumber = 0;
  snprintf(v->szCSDVersion, sizeof(v->szCSDVersion), "(native, DXVK d3d9 headless on %s)",
#ifdef __APPLE__
           "macOS"
#else
           "Linux"
#endif
           );
  return TRUE;
}
inline DWORD GetModuleFileNameA(HMODULE, char *out, DWORD n) {
  /* the scene only splits on '\\' so a POSIX path yields "" and bare
   * log/dump names land in the current directory — intended */
#ifdef __APPLE__
  uint32_t sz = n; if (_NSGetExecutablePath(out, &sz) != 0) out[0] = 0;
#else
  ssize_t k = readlink("/proc/self/exe", out, n - 1); out[k > 0 ? k : 0] = 0;
#endif
  return (DWORD)strlen(out);
}

/* the scene's ID3DXBuffer stand-in names the C vtable type; only a pointer to it is formed */
struct IUnknownVtbl;

/* DXVK throws when no Vulkan device passes its checks; the scene expects NULL */
extern "C" IDirect3D9 *Direct3DCreate9(UINT);
inline IDirect3D9 *win32hl_Direct3DCreate9(UINT v) { try { return Direct3DCreate9(v); } catch (...) { return nullptr; } }
#define Direct3DCreate9 win32hl_Direct3DCreate9
