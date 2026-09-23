/*
 * Drive the embed library's window-less Mesa backend the way the guest
 * wrapper does (mesapt_mm.c activation order), without a guest: init QEMU
 * paused, InitMesaGL → pixel format → context → draw → MGLSwapBuffers, and
 * check that on_3d_active/on_3d_frame arrive with the right pixels (green
 * clear, red quad in the top-left → verifies the bottom-up → top-down flip).
 *
 * Then the other way a guest presents: WineD3D's ddraw draws the primary
 * surface into the front buffer and flushes, it never swaps, so the frame
 * has to arrive on the flush (the guest's dispatch entries are driven here,
 * since that is where the backend's hooks live).
 *
 * Linux only (EGL backend). Build & run from the repo root:
 *   cc -O1 -std=gnu11 -Iembed -Iqemu/hw/mesa -o build/embed-3d-test \
 *      tools/embed-3d-test.c \
 *      -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,$PWD/build/qemu -lepoxy \
 *      && build/embed-3d-test
 */
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "libqemu_embed.h"
#include "mglfunci.h"     /* FEnum_*: the guest's dispatch slots */

/* backend entry points (exported by the .so on Linux; internal API) */
int InitMesaGL(void);
void FiniMesaGL(void);
void MGLTmpContext(void);
int MGLChoosePixelFormat(void);
int MGLSetPixelFormat(int, const void *);
int MGLCreateContext(uint32_t);
int MGLMakeCurrent(uint32_t, int);
int MGLSwapBuffers(void);
void MGLDeleteContext(int);
void MGLWndRelease(void);
int glwnd_ready(void);
void *MesaGLSetFunc(int fenum, void *fn);

/* what the guest's call to <fenum> reaches: the backend's hook, if it
 * installed one */
static void *dispatch_of(int fenum)
{
    void *p = MesaGLSetFunc(fenum, NULL);
    MesaGLSetFunc(fenum, p);
    return p;
}

#define MESAGL_MAGIC 0x5b5eb5e5

static int actives, frames, fw, fh;
static uint32_t px_tl, px_br, px_c;
/* zero-copy: the dma-bufs the backend offered, mmap'ed for checking */
static struct { int fd; int w, h, stride; void *map; size_t len; } slots[8];
static int dmabufs, readies, last_slot = -1;
/* Per slot, whether the buffer's own memory ever changed after its first
 * frame. One blit per slot proves only that the ring was wired up: a slot
 * whose first blit lands and whose later ones stop reaching the dma-buf
 * still publishes, and on screen that is a fast flicker between the live
 * picture and a frozen one at a third of the frame rate. */
static uint32_t slot_first_c[8];
static int slot_n[8], slot_changed[8];

static int on_3d_dmabuf(void *ud, int slot, int fd, int w, int h, int stride,
                        uint32_t fourcc, uint64_t modifier)
{
    printf("on_3d_dmabuf(slot %d fd %d %dx%d stride %d fourcc %c%c%c%c modifier 0x%llx)\n",
           slot, fd, w, h, stride, fourcc & 0xff, (fourcc >> 8) & 0xff,
           (fourcc >> 16) & 0xff, (fourcc >> 24) & 0xff, (unsigned long long)modifier);
    if (slot < 0 || slot >= 8) {
        return 0;
    }
    if (slots[slot].map) {
        munmap(slots[slot].map, slots[slot].len);
        close(slots[slot].fd);
    }
    slots[slot].fd = fd;
    slots[slot].w = w; slots[slot].h = h; slots[slot].stride = stride;
    slots[slot].len = (size_t)stride * h;
    slots[slot].map = mmap(NULL, slots[slot].len, PROT_READ, MAP_SHARED, fd, 0);
    if (slots[slot].map == MAP_FAILED) {
        printf("  mmap failed; declining zero-copy\n");
        slots[slot].map = NULL;
        return 0;
    }
    dmabufs++;
    return 1;
}

static void on_3d_frame_ready(void *ud, int slot)
{
    readies++;
    last_slot = slot;
    if (slot >= 0 && slot < 8 && slots[slot].map) {
        const uint8_t *p = slots[slot].map;
        int w = slots[slot].w, h = slots[slot].h, st = slots[slot].stride;
        frames++;
        fw = w;
        fh = h;
        px_tl = *(const uint32_t *)(p + 10 * st + 10 * 4);
        px_br = *(const uint32_t *)(p + (h - 10) * st + (w - 10) * 4);
        px_c = *(const uint32_t *)(p + (h / 2) * st + (w / 2) * 4);
        if (slot_n[slot]++ == 0) {
            slot_first_c[slot] = px_c;
        } else if (px_c != slot_first_c[slot]) {
            slot_changed[slot] = 1;
        }
    }
}

static void on_3d_active(void *ud, bool on)
{
    actives++;
    printf("on_3d_active(%d)\n", on);
}

static void on_3d_frame(void *ud, const uint8_t *p, int w, int h, int stride)
{
    const uint32_t *row = (const uint32_t *)p;
    frames++;
    fw = w;
    fh = h;
    px_tl = row[10 * (stride / 4) + 10];
    px_br = row[(h - 10) * (stride / 4) + (w - 10)];
    px_c = row[(h / 2) * (stride / 4) + (w / 2)];
}

int main(int argc, char **argv)
{
    const char *bios = argc > 1 ? argv[1] : "qemu/pc-bios";
    char *qargv[] = {
        "qemu-system-i386", "-machine", "pc", "-m", "32", "-net", "none",
        "-L", (char *)bios, "-nodefaults", "-vga", "std",
    };
    int want_zc = !(argc > 2 && !strcmp(argv[2], "readback"));
    qemu_embed_display_cb cb = {
        .on_3d_active = on_3d_active,
        .on_3d_frame = on_3d_frame,
        .on_3d_dmabuf = want_zc ? on_3d_dmabuf : NULL,
        .on_3d_frame_ready = want_zc ? on_3d_frame_ready : NULL,
    };
    if (qemu_embed_api_version() != QEMU_EMBED_API_VERSION) {
        printf("API version mismatch\n");
        return 1;
    }
    qemu_embed_t *e = qemu_embed_new(sizeof(qargv) / sizeof(qargv[0]), qargv, &cb, NULL);
    if (!e) {
        printf("qemu_embed_new failed\n");
        return 1;
    }

    /* BQL is held by this thread after init; mirror mesapt_mm.c */
    if (InitMesaGL() != 0) {
        printf("InitMesaGL failed (no libGL?)\n");
        return 1;
    }
    MGLTmpContext();
    int pf = MGLChoosePixelFormat();
    uint8_t pfd[64] = {0};
    int spf = MGLSetPixelFormat(pf, pfd);
    printf("pixel format %d set %d wnd_ready %d\n", pf, spf, glwnd_ready());
    int cc = MGLCreateContext(MESAGL_MAGIC);
    printf("MGLCreateContext -> %d (0 = ok)\n", cc);
    if (cc) {
        return 1;
    }
    MGLMakeCurrent(MESAGL_MAGIC, 0);
    printf("GL %s\n", (const char *)glGetString(GL_VERSION));

    glClearColor(0.f, 1.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    /* red quad over the top-left quadrant (GL y up: top half is y > 0) */
    glColor3f(1.f, 0.f, 0.f);
    glBegin(GL_QUADS);
    glVertex2f(-1.f, 0.f);
    glVertex2f(0.f, 0.f);
    glVertex2f(0.f, 1.f);
    glVertex2f(-1.f, 1.f);
    glEnd();
    MGLSwapBuffers();
    printf("frame %dx%d  top-left %08x  bottom-right %08x  center %08x\n",
           fw, fh, px_tl, px_br, px_c);
    int ok = frames == 1 && actives >= 1
          && (px_tl & 0xffffff) == 0xff0000
          && (px_br & 0xffffff) == 0x00ff00
          && (px_c & 0xffffff) == 0x00ff00;

    /* second frame: everything green (the pbuffer persists, no flicker) */
    glClear(GL_COLOR_BUFFER_BIT);
    MGLSwapBuffers();
    ok = ok && frames == 2 && (px_tl & 0xffffff) == 0x00ff00;

    /* Several frames per slot, each a different colour: every slot the ring
     * hands out must show the new one in its own memory. */
    for (int i = 0; i < 4 * 8; i++) {
        glClearColor(0.f, 0.f, (float)(i % 8) / 8.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        MGLSwapBuffers();
    }
    for (int i = 0; i < 8; i++) {
        if (slot_n[i] >= 2 && !slot_changed[i]) {
            printf("slot %d: %d frames blitted into it, its memory never changed"
                   " (frozen at %08x)\n", i, slot_n[i], slot_first_c[i]);
            ok = 0;
        }
    }

    /* Front-buffer presentation: no swap, a flush with the front buffer
     * selected. Magenta, so no earlier frame's colour can pass for it. */
    void (*guest_draw_buffer)(GLenum) = dispatch_of(FEnum_glDrawBuffer);
    void (*guest_flush)(void) = dispatch_of(FEnum_glFlush);
    int before = frames;
    guest_draw_buffer(GL_FRONT);
    GLenum err = glGetError();
    glClearColor(1.f, 0.f, 1.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    guest_flush();
    printf("front buffer: glDrawBuffer(GL_FRONT) err 0x%x, frames %d -> %d, "
           "center %08x\n", err, before, frames, px_c);
    if (frames != before + 1 || (px_c & 0xffffff) != 0xff00ff) {
        printf("  the frame a front-buffer flush presents never arrived\n");
        ok = 0;
    }
    guest_draw_buffer(GL_BACK);

    MGLDeleteContext(0);
    MGLWndRelease();
    FiniMesaGL();
    printf("actives %d frames %d dmabufs %d ready %d (last slot %d) -> %s [%s]\n",
           actives, frames, dmabufs, readies, last_slot, ok ? "OK" : "FAIL",
           dmabufs ? "zero-copy" : "readback");
    fflush(stdout);
    /* one VM per process; cleanup is partial, so exit without it */
    _exit(ok ? 0 : 1);
}
