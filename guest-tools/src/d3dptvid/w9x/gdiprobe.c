/*
 * gdiprobe.c — which GDI drawing operation does this driver get wrong?
 * (doc 19 §27)
 *
 * Crimson Skies draws its title screen with **no Direct3D and no DirectDraw
 * blit** — the HAL's `Blt32`, `Lock32` and `SetColorKey32` are never called
 * — and every foreground element lands in VRAM as `0xffff`, every bit set,
 * in the correct silhouette. That leaves GDI through the DIB Engine, which
 * is a large surface with no test of its own: every drawing export this
 * driver has jumps straight to the Engine (dibthunk.asm), so a fault is in
 * what the Engine was *told* about the surface rather than in code we run
 * per pixel, and the desktop exercises only a fraction of it.
 *
 * So this walks the operations a 2D game of the era composites with, one at
 * a time, and checks each one's pixels itself with `GetPixel` rather than
 * trusting a screenshot. An all-ones result is the signature of a raster op
 * or a fill going wrong rather than a copy, so the raster ops come first.
 *
 * It draws onto the **screen DC** on purpose: a memory DC would exercise the
 * Engine's own bitmaps and not this driver's PDEVICE, which is the thing
 * under test. Every case restores what it painted over.
 *
 * `gdiprobe: N cases, M failed` is the verdict, and C:\GDIPROBE.LOG has one
 * line per case with the colour asked for and the colour read back. Run it
 * with tools/win98-game-test.sh:
 *
 *   GUEST_CMD='C:\GDIPROBE.EXE' PULL=GDIPROBE.LOG \
 *     tools/win98-game-test.sh <image> gdi
 *
 * Build: guest-tools/build-driver9x.sh (mingw-w64, i686, msvcrt).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static FILE *log_file;
static int cases, failed;

static void logf_(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (log_file) {
        vfprintf(log_file, fmt, ap);
        fflush(log_file);
    }
    va_end(ap);
}

/* GetPixel returns a COLORREF (0x00bbggrr); the driver's surface is 16 bpp
 * in the interesting case, so a value is compared with the tolerance one
 * 5-6-5 step costs rather than for equality. */
static int near_(COLORREF got, COLORREF want)
{
    int dr = (int)(got & 0xff) - (int)(want & 0xff);
    int dg = (int)((got >> 8) & 0xff) - (int)((want >> 8) & 0xff);
    int db = (int)((got >> 16) & 0xff) - (int)((want >> 16) & 0xff);

    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return dr <= 12 && dg <= 12 && db <= 12;
}

static void check(const char *what, HDC dc, int x, int y, COLORREF want)
{
    COLORREF got = GetPixel(dc, x, y);

    cases++;
    if (!near_(got, want)) {
        failed++;
    }
    logf_("%-46s (%3d,%3d) got %06lx want %06lx%s%s\n", what, x, y,
          (unsigned long)got, (unsigned long)want,
          near_(got, want) ? "  PASS" : "  FAIL",
          got == 0x00ffffffUL && want != 0x00ffffffUL ? "  (all ones)" : "");
}

/* A memory bitmap the size of the test square, filled with one colour, and
 * a monochrome one for the mask passes. */
static HBITMAP solid_bitmap(HDC dc, int w, int h, COLORREF c)
{
    HBITMAP bm = CreateCompatibleBitmap(dc, w, h);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP old = SelectObject(mem, bm);
    HBRUSH br = CreateSolidBrush(c);
    RECT r;

    r.left = 0; r.top = 0; r.right = w; r.bottom = h;
    FillRect(mem, &r, br);
    DeleteObject(br);
    SelectObject(mem, old);
    DeleteDC(mem);
    return bm;
}

/* Half black, half white, 1 bpp — an AND mask's shape. */
static HBITMAP mask_bitmap(int w, int h)
{
    HBITMAP bm = CreateBitmap(w, h, 1, 1, NULL);
    HDC mem = CreateCompatibleDC(NULL);
    HBITMAP old = SelectObject(mem, bm);
    RECT r;

    r.left = 0; r.top = 0; r.right = w; r.bottom = h;
    FillRect(mem, &r, GetStockObject(WHITE_BRUSH));
    r.right = w / 2;
    FillRect(mem, &r, GetStockObject(BLACK_BRUSH));
    SelectObject(mem, old);
    DeleteDC(mem);
    return bm;
}

#define X 40
#define Y 40
#define W 80
#define H 60

static void fill(HDC dc, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    RECT r;

    r.left = X; r.top = Y; r.right = X + W; r.bottom = Y + H;
    FillRect(dc, &r, br);
    DeleteObject(br);
}

static void battery(void)
{
    HDC dc;
    HBITMAP bm, mask;
    HDC mem;
    HBITMAP old;
    int bpp;

    dc = GetDC(NULL);
    if (!dc) {
        logf_("gdiprobe: no screen DC\n");
        return;
    }
    bpp = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);
    logf_("gdiprobe: screen %dx%d %d bpp, RASTERCAPS %04x\n",
          GetDeviceCaps(dc, HORZRES), GetDeviceCaps(dc, VERTRES), bpp,
          GetDeviceCaps(dc, RASTERCAPS));

    /* 1. a solid fill, which everything else is measured against */
    fill(dc, RGB(0, 0, 255));
    check("FillRect(blue)", dc, X + 10, Y + 10, RGB(0, 0, 255));

    /* 2. PatBlt's three constant operations */
    fill(dc, RGB(0, 0, 255));
    PatBlt(dc, X, Y, W, H, BLACKNESS);
    check("PatBlt(BLACKNESS)", dc, X + 10, Y + 10, RGB(0, 0, 0));
    PatBlt(dc, X, Y, W, H, WHITENESS);
    check("PatBlt(WHITENESS)", dc, X + 10, Y + 10, RGB(255, 255, 255));

    /* 3. SRCCOPY from a compatible bitmap — the plain copy */
    bm = solid_bitmap(dc, W, H, RGB(255, 128, 0));
    mem = CreateCompatibleDC(dc);
    old = SelectObject(mem, bm);
    fill(dc, RGB(0, 0, 255));
    BitBlt(dc, X, Y, W, H, mem, 0, 0, SRCCOPY);
    check("BitBlt(SRCCOPY, orange)", dc, X + 10, Y + 10, RGB(255, 128, 0));

    /* 4. the raster ops a masked sprite is composited with. AND with white
     * leaves the destination; OR with black leaves it; that is the whole
     * idiom, and each half is checked on its own so a failure names itself. */
    fill(dc, RGB(0, 0, 255));
    SelectObject(mem, old);
    DeleteObject(bm);
    bm = solid_bitmap(dc, W, H, RGB(255, 255, 255));
    old = SelectObject(mem, bm);
    BitBlt(dc, X, Y, W, H, mem, 0, 0, SRCAND);
    check("BitBlt(SRCAND, white) keeps the blue", dc, X + 10, Y + 10, RGB(0, 0, 255));

    fill(dc, RGB(0, 0, 255));
    SelectObject(mem, old);
    DeleteObject(bm);
    bm = solid_bitmap(dc, W, H, RGB(0, 0, 0));
    old = SelectObject(mem, bm);
    BitBlt(dc, X, Y, W, H, mem, 0, 0, SRCPAINT);
    check("BitBlt(SRCPAINT, black) keeps the blue", dc, X + 10, Y + 10, RGB(0, 0, 255));

    fill(dc, RGB(0, 0, 255));
    SelectObject(mem, old);
    DeleteObject(bm);
    bm = solid_bitmap(dc, W, H, RGB(255, 0, 0));
    old = SelectObject(mem, bm);
    BitBlt(dc, X, Y, W, H, mem, 0, 0, SRCPAINT);
    check("BitBlt(SRCPAINT, red) over blue -> magenta", dc, X + 10, Y + 10, RGB(255, 0, 255));

    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);

    /* 5. the two-pass transparent sprite, the thing Crimson Skies' title
     * screen is made of: AND a monochrome mask, then OR the image. The left
     * half of the mask is black (opaque) and the right half white
     * (transparent), so the left half must end up green and the right half
     * must still be the blue underneath. */
    {
        HDC maskdc = CreateCompatibleDC(dc), imgdc = CreateCompatibleDC(dc);
        HBITMAP img = solid_bitmap(dc, W, H, RGB(0, 255, 0));
        HBITMAP oldm, oldi;

        mask = mask_bitmap(W, H);
        oldm = SelectObject(maskdc, mask);
        oldi = SelectObject(imgdc, img);

        fill(dc, RGB(0, 0, 255));
        BitBlt(dc, X, Y, W, H, maskdc, 0, 0, SRCAND);
        check("sprite pass 1: mask AND, opaque half black", dc, X + 10, Y + 10, RGB(0, 0, 0));
        check("sprite pass 1: mask AND, clear half kept", dc, X + W - 10, Y + 10, RGB(0, 0, 255));

        /* the image is green everywhere here, so the transparent half is
         * deliberately blacked out first, exactly as a real sprite sheet is */
        BitBlt(imgdc, W / 2, 0, W / 2, H, imgdc, W / 2, 0, BLACKNESS);
        BitBlt(dc, X, Y, W, H, imgdc, 0, 0, SRCPAINT);
        check("sprite pass 2: image OR, opaque half green", dc, X + 10, Y + 10, RGB(0, 255, 0));
        check("sprite pass 2: image OR, clear half kept", dc, X + W - 10, Y + 10, RGB(0, 0, 255));

        SelectObject(maskdc, oldm);
        SelectObject(imgdc, oldi);
        DeleteObject(mask);
        DeleteObject(img);
        DeleteDC(maskdc);
        DeleteDC(imgdc);
    }

    /* 6. StretchBlt, which a title screen scaled to the desktop uses */
    {
        HDC sdc = CreateCompatibleDC(dc);
        HBITMAP sbm = solid_bitmap(dc, W / 2, H / 2, RGB(0, 255, 255));
        HBITMAP olds = SelectObject(sdc, sbm);

        fill(dc, RGB(0, 0, 255));
        StretchBlt(dc, X, Y, W, H, sdc, 0, 0, W / 2, H / 2, SRCCOPY);
        check("StretchBlt(SRCCOPY, cyan) 2x", dc, X + 10, Y + 10, RGB(0, 255, 255));
        SelectObject(sdc, olds);
        DeleteObject(sbm);
        DeleteDC(sdc);
    }

    /* 7. SetDIBitsToDevice at the three depths a game ships art in. The DIB
     * is one colour so a wrong stride shows as the wrong colour rather than
     * as a shear nobody can read from one pixel. */
    {
        static const struct { int bits; const char *name; } depths[] = {
            { 24, "24 bpp" }, { 32, "32 bpp" }, { 16, "16 bpp BI_RGB (5-5-5)" }
        };
        int d;

        for (d = 0; d < 3; d++) {
            BITMAPINFO *bi = (BITMAPINFO *)calloc(1, sizeof(BITMAPINFOHEADER) + 16);
            int stride = ((W * depths[d].bits + 31) / 32) * 4;
            unsigned char *bits = (unsigned char *)calloc(1, (size_t)stride * H);
            int i, j;
            char label[80];

            bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi->bmiHeader.biWidth = W;
            bi->bmiHeader.biHeight = H;          /* bottom-up */
            bi->bmiHeader.biPlanes = 1;
            bi->bmiHeader.biBitCount = (WORD)depths[d].bits;
            bi->bmiHeader.biCompression = BI_RGB;

            /* pure red in each depth's own layout */
            for (j = 0; j < H; j++) {
                unsigned char *row = bits + (size_t)stride * j;
                for (i = 0; i < W; i++) {
                    if (depths[d].bits == 24) {
                        row[i * 3 + 0] = 0; row[i * 3 + 1] = 0; row[i * 3 + 2] = 255;
                    } else if (depths[d].bits == 32) {
                        row[i * 4 + 0] = 0; row[i * 4 + 1] = 0; row[i * 4 + 2] = 255; row[i * 4 + 3] = 0;
                    } else {
                        unsigned short v = (unsigned short)(31 << 10);   /* 5-5-5 red */
                        row[i * 2 + 0] = (unsigned char)(v & 0xff);
                        row[i * 2 + 1] = (unsigned char)(v >> 8);
                    }
                }
            }
            fill(dc, RGB(0, 0, 255));
            SetDIBitsToDevice(dc, X, Y, W, H, 0, 0, 0, H, bits, bi, DIB_RGB_COLORS);
            sprintf(label, "SetDIBitsToDevice(%s, red)", depths[d].name);
            check(label, dc, X + 10, Y + 10, RGB(255, 0, 0));
            free(bits);
            free(bi);
        }
    }

    /* 8. **Reading the screen back**, which is the half of compositing the
     * cases above never touch. Anything that blends — msimg32's AlphaBlend,
     * a TransparentBlt emulation, a game's own "read the background, mix,
     * write it back" — reads the destination first, and a read that returns
     * all-ones turns every blend white while leaving plain copies perfect.
     * That is the shape of the bug this probe was written for. */
    {
        BITMAPINFO *bi = (BITMAPINFO *)calloc(1, sizeof(BITMAPINFOHEADER) + 16);
        int stride = ((W * 24 + 31) / 32) * 4;
        unsigned char *bits = (unsigned char *)calloc(1, (size_t)stride * H);
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP mb = CreateCompatibleBitmap(dc, W, H);
        HBITMAP oldb = SelectObject(mdc, mb);
        int got;

        fill(dc, RGB(0, 128, 255));

        bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi->bmiHeader.biWidth = W;
        bi->bmiHeader.biHeight = H;
        bi->bmiHeader.biPlanes = 1;
        bi->bmiHeader.biBitCount = 24;
        bi->bmiHeader.biCompression = BI_RGB;
        got = GetDIBits(dc, GetCurrentObject(dc, OBJ_BITMAP), 0, H, bits, bi, DIB_RGB_COLORS);
        logf_("GetDIBits(screen, 24 bpp) -> %d rows\n", got);

        /* BitBlt the screen into a memory bitmap and read *that* back, which
         * is what every compositor actually does. */
        BitBlt(mdc, 0, 0, W, H, dc, X, Y, SRCCOPY);
        memset(bits, 0, (size_t)stride * H);
        got = GetDIBits(mdc, mb, 0, H, bits, bi, DIB_RGB_COLORS);
        cases++;
        {
            /* bottom-up: row 0 of the buffer is the bottom line */
            unsigned char *px = bits + (size_t)stride * (H - 1 - 10) + 10 * 3;
            COLORREF back = RGB(px[2], px[1], px[0]);

            if (!near_(back, RGB(0, 128, 255))) failed++;
            logf_("%-46s          got %06lx want %06lx%s%s\n",
                  "BitBlt(screen->mem) + GetDIBits", (unsigned long)back,
                  (unsigned long)RGB(0, 128, 255),
                  near_(back, RGB(0, 128, 255)) ? "  PASS" : "  FAIL",
                  back == 0x00ffffffUL ? "  (all ones)" : "");
        }

        /* and straight back out again — a full read/modify/write round trip */
        fill(dc, RGB(0, 0, 0));
        BitBlt(dc, X, Y, W, H, mdc, 0, 0, SRCCOPY);
        check("round trip: screen -> mem -> screen", dc, X + 10, Y + 10, RGB(0, 128, 255));

        SelectObject(mdc, oldb);
        DeleteObject(mb);
        DeleteDC(mdc);
        free(bits);
        free(bi);
    }

    /* 9. StretchDIBits, the other DIB entry, and a pattern brush, which is
     * how a tiled or textured fill reaches the Engine. */
    {
        BITMAPINFO *bi = (BITMAPINFO *)calloc(1, sizeof(BITMAPINFOHEADER) + 16);
        int stride = ((8 * 24 + 31) / 32) * 4;
        unsigned char *bits = (unsigned char *)calloc(1, (size_t)stride * 8);
        int i, j;

        bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi->bmiHeader.biWidth = 8;
        bi->bmiHeader.biHeight = 8;
        bi->bmiHeader.biPlanes = 1;
        bi->bmiHeader.biBitCount = 24;
        bi->bmiHeader.biCompression = BI_RGB;
        for (j = 0; j < 8; j++) {
            unsigned char *row = bits + (size_t)stride * j;
            for (i = 0; i < 8; i++) { row[i*3+0] = 255; row[i*3+1] = 0; row[i*3+2] = 128; }
        }
        fill(dc, RGB(0, 0, 0));
        StretchDIBits(dc, X, Y, W, H, 0, 0, 8, 8, bits, bi, DIB_RGB_COLORS, SRCCOPY);
        check("StretchDIBits(24 bpp) scaled up", dc, X + 10, Y + 10, RGB(128, 0, 255));
        free(bits);
        free(bi);
    }
    {
        HBITMAP pat = solid_bitmap(dc, 8, 8, RGB(0, 200, 100));
        HBRUSH br = CreatePatternBrush(pat);
        HBRUSH oldbr = SelectObject(dc, br);

        fill(dc, RGB(0, 0, 0));
        PatBlt(dc, X, Y, W, H, PATCOPY);
        check("PatBlt(PATCOPY, pattern brush)", dc, X + 10, Y + 10, RGB(0, 200, 100));
        SelectObject(dc, oldbr);
        DeleteObject(br);
        DeleteObject(pat);
    }

    fill(dc, RGB(0, 0, 0));
    ReleaseDC(NULL, dc);
}

/* **The depth is the variable that has to be controlled, not assumed.** The
 * first A/B of this bug compared our driver against the in-box Cirrus and
 * never established that both were at the same colour depth — two variables
 * in a two-column table. So the probe changes the depth itself and runs the
 * whole battery again, which also answers "is this specific to 16 bpp" in
 * one boot instead of two.
 *
 * `ChangeDisplaySettings` is the only way to do this from outside the
 * guest's UI: the display driver's INF resets the mode only when PnP
 * actually reinstalls the device, and on a machine already bound to the
 * driver a reinstall changes nothing (doc 19 §27). */
static void at_depth(int bpp)
{
    DEVMODE dm;
    LONG rc;

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmBitsPerPel = bpp;
    dm.dmFields = DM_BITSPERPEL;
    rc = ChangeDisplaySettings(&dm, 0);
    logf_("\ngdiprobe: ChangeDisplaySettings(%d bpp) -> %ld\n", bpp, (long)rc);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        return;
    }
    Sleep(1500);
    battery();
}

int main(void)
{
    int first_bpp;
    HDC dc;

    log_file = fopen("C:\\GDIPROBE.LOG", "w");

    dc = GetDC(NULL);
    first_bpp = dc ? GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES) : 0;
    if (dc) ReleaseDC(NULL, dc);

    battery();
    if (first_bpp != 16) at_depth(16);
    if (first_bpp != 32) at_depth(32);
    if (first_bpp != 8)  at_depth(8);

    logf_("\ngdiprobe: %d cases, %d failed\n", cases, failed);
    logf_("gdiprobe: done\n");
    if (log_file) fclose(log_file);
    return 0;
}
