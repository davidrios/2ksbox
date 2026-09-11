/*
 * patchtst.c — higher-order surfaces through XP's own d3d8.dll on our
 * driver's DX8 DDI: RT-patches (DrawRectPatch of a cubic Bezier patch) and
 * N-patches (D3DRS_PATCHSEGMENTS over an ordinary triangle list). A probe
 * (d3d8probe.h): with neither D3DDEVCAPS_RTPATCHES nor
 * D3DDEVCAPS_NPATCHES it says "not offered"; what is offered is checked,
 * the rest logged and skipped. Both surfaces are flat, so tessellated
 * they cover exactly what their control points do: the check is that they
 * are drawn at all, green in the middle and nothing outside.
 *
 *   PATCHTST
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3d8probe.h"

struct pc_vtx { float x, y, z; DWORD color; };
struct pn_vtx { float x, y, z, nx, ny, nz; DWORD color; };

static void rt_patch(void)
{
    IDirect3DVertexBuffer8 *vb = NULL;
    struct pc_vtx *p = NULL;
    D3DRECTPATCH_INFO info;
    float segs[4] = { 4.0f, 4.0f, 4.0f, 4.0f };
    UINT handle = (caps.DevCaps & D3DDEVCAPS_RTPATCHHANDLEZERO) ? 0 : 1;
    int i, j, xs[2] = { 160, 20 }, ys[2] = { 120, 20 };
    DWORD w[2] = { 0xff00ff00, CLEAR_COLOR };
    HRESULT hr = IDirect3DDevice8_CreateVertexBuffer(dev, 16 * sizeof(struct pc_vtx), D3DUSAGE_WRITEONLY | D3DUSAGE_RTPATCHES,
                                                     D3DFVF_XYZ | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb);

    logp("CreateVertexBuffer (16 control points) 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr) || !vb || FAILED(IDirect3DVertexBuffer8_Lock(vb, 0, 0, (BYTE **)&p, 0))) {
        fail_case("RT-patch: the control points");
        if (vb) IDirect3DVertexBuffer8_Release(vb);
        return;
    }
    /* a flat 4 x 4 grid over the middle of clip space */
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++) {
            p[j * 4 + i].x = -0.5f + i / 3.0f;
            p[j * 4 + i].y = 0.5f - j / 3.0f;
            p[j * 4 + i].z = 0.5f;
            p[j * 4 + i].color = 0xff00ff00;
        }
    IDirect3DVertexBuffer8_Unlock(vb);
    info.StartVertexOffsetWidth = 0;
    info.StartVertexOffsetHeight = 0;
    info.Width = 4;
    info.Height = 4;
    info.Stride = 4;
    info.Basis = D3DBASIS_BEZIER;
    info.Order = D3DORDER_CUBIC;
    IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    IDirect3DDevice8_SetStreamSource(dev, 0, vb, sizeof(struct pc_vtx));
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    begin();
    hr = IDirect3DDevice8_DrawRectPatch(dev, handle, segs, &info);
    end();
    logp("DrawRectPatch (handle %u, cubic Bezier, 4 segments a side) 0x%08lx\n", handle, (unsigned long)hr);
    check("RT-patch: a flat cubic Bezier patch drawn", 2, xs, ys, w);
    if (handle) IDirect3DDevice8_DeletePatch(dev, handle);
    IDirect3DDevice8_SetStreamSource(dev, 0, NULL, 0);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DVertexBuffer8_Release(vb);
}

static void n_patch(void)
{
    struct pn_vtx q[6] = {
        { -0.5f, 0.5f, 0.5f, 0, 0, -1, 0xff00ff00 }, { 0.5f, 0.5f, 0.5f, 0, 0, -1, 0xff00ff00 }, { -0.5f, -0.5f, 0.5f, 0, 0, -1, 0xff00ff00 },
        { -0.5f, -0.5f, 0.5f, 0, 0, -1, 0xff00ff00 }, { 0.5f, 0.5f, 0.5f, 0, 0, -1, 0xff00ff00 }, { 0.5f, -0.5f, 0.5f, 0, 0, -1, 0xff00ff00 },
    };
    int xs[2] = { 160, 20 }, ys[2] = { 120, 20 };
    DWORD w[2] = { 0xff00ff00, CLEAR_COLOR };
    HRESULT hr;

    IDirect3DDevice8_SetVertexShader(dev, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_PATCHSEGMENTS, fbits(4.0f));
    begin();
    hr = IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, q, sizeof q[0]);
    end();
    logp("DrawPrimitiveUP (PATCHSEGMENTS 4) 0x%08lx\n", (unsigned long)hr);
    check("N-patches: a flat quad under PATCHSEGMENTS 4 drawn", 2, xs, ys, w);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_PATCHSEGMENTS, fbits(1.0f));
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
}

int main(void)
{
    int rt, np;

    if (!probe_open("patchtst")) return 1;
    rt = (caps.DevCaps & D3DDEVCAPS_RTPATCHES) != 0;
    np = (caps.DevCaps & D3DDEVCAPS_NPATCHES) != 0;
    logp("patchtst: RTPATCHES %s (handle zero %s), NPATCHES %s\n", rt ? "yes" : "no",
         (caps.DevCaps & D3DDEVCAPS_RTPATCHHANDLEZERO) ? "yes" : "no", np ? "yes" : "no");
    if (!rt && !np) {
        return probe_close("neither D3DDEVCAPS_RTPATCHES nor D3DDEVCAPS_NPATCHES");
    }
    if (!probe_device()) {
        fail_case("the device");
        return probe_close(NULL);
    }
    if (rt) rt_patch(); else logp("RT-patches: not in the caps, skipped\n");
    if (np) n_patch(); else logp("N-patches: not in the caps, skipped\n");
    return probe_close(NULL);
}
