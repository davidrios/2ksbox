/*
 * glprobe.c: GLPROBE.EXE, which OpenGL a program on this machine gets.
 *
 * The DirectDraw counterpart of this is DDPROBE.EXE, and the question is the
 * same shape: not "does OpenGL work" but *whose* OpenGL answered. On these
 * machines there are two, and they are told apart by one string:
 *
 *   GL_RENDERER "GDI Generic"   Microsoft's software GL 1.1: every frame
 *                               drawn by the guest CPU, far too slow to play
 *   anything else               qemu-3dfx's pass-through (OPENGL32.DLL),
 *                               which is the host's GL through the player
 *
 * It matters because the pass-through is reached by *name*: the first
 * opengl32.dll the loader finds is the one a program draws through, so a GL
 * title gets the pass-through only if a copy sits next to its EXE, or if the
 * one in the system folder is ours.
 *
 * The pass-through refuses to load without the device mapper (FXMEMMAP.VXD
 * on 9x, the MAPMEM service on NT): its DllMain returns FALSE, and a program
 * that imports opengl32 then does not start. This probe links GL at run time
 * (LoadLibrary, not an import), so it reports the refusal rather than dying
 * before main().
 *
 * It draws: a context on an ordinary window, a clear to a known colour read
 * back with glReadPixels (a GL that answers strings but draws nothing has
 * happened here), then N frames of a spinning quad with
 * SwapBuffers, and the frame rate. Everything to C:\GLPROBE.LOG, so WIN.INI's
 * `run=` can start it with nothing to type at.
 *
 *   GLPROBE.EXE [frames]        (default 120)
 *
 * Build: guest-tools/build-wrappers.sh (mingw-w64, i686, msvcrt).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* GL, by hand: this must not import opengl32, or a machine whose
 * pass-through refuses to load would kill the probe before it ran. */
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_VENDOR           0x1F00
#define GL_RENDERER         0x1F01
#define GL_VERSION          0x1F02
#define GL_TRIANGLE_FAN     0x0006
#define GL_RGB              0x1907
#define GL_UNSIGNED_BYTE    0x1401

typedef HGLRC (WINAPI *PFNWGLCREATE)(HDC);
typedef BOOL  (WINAPI *PFNWGLMAKECUR)(HDC, HGLRC);
typedef BOOL  (WINAPI *PFNWGLDELETE)(HGLRC);
typedef const unsigned char * (WINAPI *PFNGLGETSTRING)(unsigned int);
typedef void  (WINAPI *PFNGLCLEARCOLOR)(float, float, float, float);
typedef void  (WINAPI *PFNGLCLEAR)(unsigned int);
typedef void  (WINAPI *PFNGLBEGIN)(unsigned int);
typedef void  (WINAPI *PFNGLEND)(void);
typedef void  (WINAPI *PFNGLCOLOR3F)(float, float, float);
typedef void  (WINAPI *PFNGLVERTEX2F)(float, float);
typedef void  (WINAPI *PFNGLFINISH)(void);
typedef void  (WINAPI *PFNGLREADPIXELS)(int, int, int, int, unsigned int, unsigned int, void *);

static PFNWGLCREATE     p_wglCreateContext;
static PFNWGLMAKECUR    p_wglMakeCurrent;
static PFNWGLDELETE     p_wglDeleteContext;
static PFNGLGETSTRING   p_glGetString;
static PFNGLCLEARCOLOR  p_glClearColor;
static PFNGLCLEAR       p_glClear;
static PFNGLBEGIN       p_glBegin;
static PFNGLEND         p_glEnd;
static PFNGLCOLOR3F     p_glColor3f;
static PFNGLVERTEX2F    p_glVertex2f;
static PFNGLFINISH      p_glFinish;
static PFNGLREADPIXELS  p_glReadPixels;

static FILE *out;

static void logp(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (out) {
        vfprintf(out, fmt, ap);
        fputc('\n', out);
        fflush(out);
    }
    va_end(ap);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(w, m, wp, lp);
}

int main(int argc, char **argv)
{
    static PIXELFORMATDESCRIPTOR pfd;
    HMODULE gl;
    HWND win;
    HDC dc;
    HGLRC rc;
    WNDCLASSA wc;
    DWORD t0, t1;
    unsigned char px[3];
    char path[MAX_PATH];
    int frames = (argc > 1) ? atoi(argv[1]) : 120, i, fmt, bad = 0;

    out = fopen("C:\\GLPROBE.LOG", "w");
    logp("glprobe: start, %d frames", frames);

    /* Which file answered, before anything is asked of it: on a machine
     * where the system OPENGL32.DLL has been replaced this is the one line
     * that says whose it is. */
    gl = LoadLibraryA("opengl32.dll");
    if (!gl) {
        logp("glprobe: opengl32.dll did not load (error %lu); on 9x that is"
             " the pass-through without its device mapper (SETUP /I 2)",
             (unsigned long)GetLastError());
        if (out) fclose(out);
        return 1;
    }
    if (GetModuleFileNameA(gl, path, sizeof(path))) {
        logp("glprobe: opengl32.dll is %s", path);
    }
    p_wglCreateContext = (PFNWGLCREATE)GetProcAddress(gl, "wglCreateContext");
    p_wglMakeCurrent   = (PFNWGLMAKECUR)GetProcAddress(gl, "wglMakeCurrent");
    p_wglDeleteContext = (PFNWGLDELETE)GetProcAddress(gl, "wglDeleteContext");
    p_glGetString      = (PFNGLGETSTRING)GetProcAddress(gl, "glGetString");
    p_glClearColor     = (PFNGLCLEARCOLOR)GetProcAddress(gl, "glClearColor");
    p_glClear          = (PFNGLCLEAR)GetProcAddress(gl, "glClear");
    p_glBegin          = (PFNGLBEGIN)GetProcAddress(gl, "glBegin");
    p_glEnd            = (PFNGLEND)GetProcAddress(gl, "glEnd");
    p_glColor3f        = (PFNGLCOLOR3F)GetProcAddress(gl, "glColor3f");
    p_glVertex2f       = (PFNGLVERTEX2F)GetProcAddress(gl, "glVertex2f");
    p_glFinish         = (PFNGLFINISH)GetProcAddress(gl, "glFinish");
    p_glReadPixels     = (PFNGLREADPIXELS)GetProcAddress(gl, "glReadPixels");
    if (!p_wglCreateContext || !p_glGetString || !p_glClear || !p_glReadPixels) {
        logp("glprobe: opengl32.dll is missing entry points");
        if (out) fclose(out);
        return 1;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "glprobe";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    win = CreateWindowExA(0, "glprobe", "glprobe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          16, 16, 320, 240, NULL, NULL, wc.hInstance, NULL);
    if (!win) {
        logp("glprobe: no window");
        if (out) fclose(out);
        return 1;
    }
    dc = GetDC(win);
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 16;
    pfd.cDepthBits = 16;
    fmt = ChoosePixelFormat(dc, &pfd);
    logp("glprobe: ChoosePixelFormat -> %d", fmt);
    if (!fmt || !SetPixelFormat(dc, fmt, &pfd)) {
        logp("glprobe: no pixel format (error %lu)", (unsigned long)GetLastError());
        if (out) fclose(out);
        return 1;
    }
    rc = p_wglCreateContext(dc);
    if (!rc || !p_wglMakeCurrent(dc, rc)) {
        logp("glprobe: no GL context (error %lu); under a bare qemu-system-i386"
             " there is no 3D provider and the pass-through refuses one by design",
             (unsigned long)GetLastError());
        if (out) fclose(out);
        return 1;
    }

    /* Whose GL this is. "GDI Generic" is Microsoft's software renderer;
     * anything else here is the pass-through answering with the host's. */
    logp("glprobe: GL_VENDOR   %s", (const char *)p_glGetString(GL_VENDOR));
    logp("glprobe: GL_RENDERER %s", (const char *)p_glGetString(GL_RENDERER));
    logp("glprobe: GL_VERSION  %s", (const char *)p_glGetString(GL_VERSION));

    /* A clear read back: a GL that answers strings and draws nothing has
     * happened here more than once (doc 12 §4). */
    p_glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glFinish();
    memset(px, 0, sizeof(px));
    p_glReadPixels(160, 120, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
    logp("glprobe: the cleared pixel reads %02x%02x%02x (want 00ff00)", px[0], px[1], px[2]);
    if (px[1] < 0xc0 || px[0] > 0x40 || px[2] > 0x40) {
        bad++;
    }

    t0 = GetTickCount();
    for (i = 0; i < frames; i++) {
        float a = (float)i * 0.05f;
        MSG msg;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        p_glClearColor(0.1f, 0.1f, 0.3f, 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glBegin(GL_TRIANGLE_FAN);
        p_glColor3f(1.0f, 0.8f, 0.2f);
        p_glVertex2f(0.0f, 0.0f);
        p_glColor3f(0.2f, 0.6f, 1.0f);
        p_glVertex2f(0.6f * (float)cos(a), 0.6f * (float)sin(a));
        p_glVertex2f(-0.6f * (float)sin(a), 0.6f * (float)cos(a));
        p_glVertex2f(-0.6f * (float)cos(a), -0.6f * (float)sin(a));
        p_glEnd();
        SwapBuffers(dc);
    }
    t1 = GetTickCount();
    logp("glprobe: %d frames in %lu ms = %.1f fps", frames, (unsigned long)(t1 - t0),
         (t1 > t0) ? (frames * 1000.0 / (double)(t1 - t0)) : 0.0);

    p_wglMakeCurrent(dc, NULL);
    p_wglDeleteContext(rc);
    ReleaseDC(win, dc);
    DestroyWindow(win);
    logp("glprobe: %d case%s failed", bad, bad == 1 ? "" : "s");
    if (out) {
        fclose(out);
    }
    return bad ? 1 : 0;
}
