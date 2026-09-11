/*
 * fmttest.c — the Direct3D 8 texture formats our driver's DX8 DDI does not
 * list yet: L8, A8L8, A4L4, A8, X4R4G4B4, R3G3B2, A8R3G3B2, DXT2, DXT4
 * (the bump-map formats are BUMPTEST's). A probe (d3d8probe.h): each format
 * the format list offers is created (MANAGED, 8x8), filled with one known
 * texel and drawn twice — its colour, and its alpha replicated into the
 * colour (D3DTA_ALPHAREPLICATE) — and both read back against what the
 * format means; a format not offered is logged and skipped. With none of
 * them offered the last line says "not offered". DXTTEST remains the
 * check of the formats that are listed.
 *
 *   FMTTEST
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

struct fmt_case {
    D3DFORMAT f;
    const char *name;
    UINT bytes;             /* per texel; 0 = a DXT block format */
    DWORD texel;            /* the texel written (for a DXT format: its colour, full alpha) */
    DWORD rgb, alpha;       /* what the colour and the replicated alpha must read as */
    int has_rgb;            /* A8 has no colour to check: what it reads as is the API's business */
};

static const struct fmt_case fmts[] = {
    { D3DFMT_L8,       "L8",       1, 0x80,       0x808080, 0xffffff, 1 },
    { D3DFMT_A8L8,     "A8L8",     2, 0x40c0,     0xc0c0c0, 0x404040, 1 },
    { D3DFMT_A4L4,     "A4L4",     1, 0x4c,       0xcccccc, 0x444444, 1 },
    { D3DFMT_A8,       "A8",       1, 0x60,       0x000000, 0x606060, 0 },
    { D3DFMT_X4R4G4B4, "X4R4G4B4", 2, 0x0f84,     0xff8844, 0xffffff, 1 },
    { D3DFMT_R3G3B2,   "R3G3B2",   1, 0xe3,       0xff00ff, 0xffffff, 1 },
    { D3DFMT_A8R3G3B2, "A8R3G3B2", 2, 0x40e3,     0xff00ff, 0x404040, 1 },
    { D3DFMT_DXT2,     "DXT2",     0, 0xffff0000, 0xff0000, 0xffffff, 1 },
    { D3DFMT_DXT4,     "DXT4",     0, 0xffff0000, 0xff0000, 0xffffff, 1 },
};

static WORD rgb565(DWORD c)
{
    return (WORD)((((c >> 19) & 0x1f) << 11) | (((c >> 10) & 0x3f) << 5) | ((c >> 3) & 0x1f));
}

/* an 8x8 texture of one texel; DXT2 / DXT4 as blocks of one colour, full alpha */
static IDirect3DTexture8 *make(const struct fmt_case *c)
{
    IDirect3DTexture8 *t = NULL;
    D3DLOCKED_RECT lr;
    UINT x, y;
    HRESULT hr = IDirect3DDevice8_CreateTexture(dev, 8, 8, 1, 0, c->f, D3DPOOL_MANAGED, &t);

    logp("CreateTexture (%s, 8x8, managed) 0x%08lx\n", c->name, (unsigned long)hr);
    if (FAILED(hr) || !t) return NULL;
    hr = IDirect3DTexture8_LockRect(t, 0, &lr, NULL, 0);
    if (FAILED(hr)) {
        logp("  LockRect 0x%08lx\n", (unsigned long)hr);
        IDirect3DTexture8_Release(t);
        return NULL;
    }
    for (y = 0; y < (c->bytes ? 8u : 2u); y++) {
        BYTE *row = (BYTE *)lr.pBits + y * lr.Pitch;
        for (x = 0; x < (c->bytes ? 8u : 2u); x++) {
            if (c->bytes == 1) {
                row[x] = (BYTE)c->texel;
            } else if (c->bytes == 2) {
                WORD v = (WORD)c->texel;
                memcpy(row + x * 2, &v, 2);
            } else {
                /* a 16-byte block: the alpha half (DXT2: 4-bit explicit
                 * alphas, all 15; DXT4: two endpoints of 255 and indices 0),
                 * then the colour half (both endpoints the colour, indices 0) */
                BYTE *b = row + x * 16;
                WORD c565 = rgb565(c->texel);
                if (c->f == D3DFMT_DXT2) {
                    memset(b, 0xff, 8);
                } else {
                    memset(b, 0, 8);
                    b[0] = 0xff;
                    b[1] = 0xff;
                }
                memcpy(b + 8, &c565, 2);
                memcpy(b + 10, &c565, 2);
                memset(b + 12, 0, 4);
            }
        }
    }
    IDirect3DTexture8_UnlockRect(t, 0);
    return t;
}

int main(void)
{
    char absent[256] = "";
    unsigned i, offered = 0;

    if (!probe_open("fmttest")) return 1;
    for (i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        if (SUCCEEDED(probe_format(fmts[i].name, D3DRTYPE_TEXTURE, 0, fmts[i].f))) {
            offered++;
        } else {
            strncat(absent, " ", sizeof absent - strlen(absent) - 1);
            strncat(absent, fmts[i].name, sizeof absent - strlen(absent) - 1);
        }
    }
    if (!offered) {
        return probe_close("none of L8 A8L8 A4L4 A8 X4R4G4B4 R3G3B2 A8R3G3B2 DXT2 DXT4 in the format list");
    }
    if (!probe_device()) {
        fail_case("the device");
        return probe_close(NULL);
    }
    for (i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        const struct fmt_case *c = &fmts[i];
        IDirect3DTexture8 *t;
        char name[80];
        int xs[2] = { 70, 210 }, ys[2] = { 70, 70 };
        DWORD w[2];

        if (FAILED(IDirect3D8_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format, 0, D3DRTYPE_TEXTURE, c->f))) {
            continue;
        }
        snprintf(name, sizeof name, "%s: its colour and its alpha", c->name);
        t = make(c);
        if (!t) {
            fail_case(name);
            continue;
        }
        IDirect3DDevice8_SetTexture(dev, 0, (IDirect3DBaseTexture8 *)t);
        begin();
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        quad_uv(40.0f, 40.0f, 60.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE);
        quad_uv(180.0f, 40.0f, 60.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        end();
        w[0] = c->rgb;
        w[1] = c->alpha;
        if (c->has_rgb) {
            check(name, 2, xs, ys, w);
        } else {
            check(name, 1, xs + 1, ys + 1, w + 1);
        }
        IDirect3DDevice8_SetTexture(dev, 0, NULL);
        IDirect3DTexture8_Release(t);
    }
    if (absent[0]) {
        logp("fmttest: not in the format list:%s\n", absent);
    }
    return probe_close(NULL);
}
