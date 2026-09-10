/*
 * bsodvxd.c — a VxD whose only job is to blue-screen Windows 98 (doc 19
 * §29). Loaded dynamically by bsod.exe (`\\.\C:\BSODVXD.VXD`); its
 * Sys_Dynamic_Device_Init executes an invalid opcode in ring 0, and the VMM
 * puts up its fatal-exception screen ("exception 06 ... in VxD BSODVXD(01)
 * ... press any key to attempt to continue") — the message mode whose
 * visibility tools/win98-bsod-test.sh guards.
 *
 * Why a VxD of our own rather than a Windows bug: the `con\con` path that
 * every write-up names is patched on the test image (bsod.c tries it first
 * and says so), and a trigger the test depends on has to be something we
 * ship. Nothing here touches the adapter; the mini-VDD is the one that has
 * to notice (SAVE_MESSAGE_MODE_STATE).
 *
 * Built by guest-tools/build-driver9x.sh with the same recipe as the
 * mini-VDD: one CODE-class segment so the DDB is at offset 0 of object 1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "winhack.h"
#include "vmm.h"

#pragma data_seg("_LTEXT", "CODE")
#pragma code_seg("_LTEXT", "CODE")
#pragma const_seg("_LTEXT", "CODE")

void VXD_control(void);

DDB VXD_DDB = {
    NULL,
    DDK_VERSION,
    0x4335,                     /* one past the mini-VDD's, unassigned */
    1, 0,
    0,
    "BSODVXD ",
    Undefined_Init_Order,
    (DWORD)VXD_control,
    NULL, NULL,
    NULL, NULL,
    NULL,
    NULL, 0,
    NULL,
    'Prev',
    sizeof(DDB),
    'Rsv1', 'Rsv2', 'Rsv3',
};

/* Dynamic init: the fault. ud2 is the invalid opcode, chosen over a bad
 * memory access because linear 0 is a VM's V86 page on 9x and reads fine. */
void __declspec(naked) VXD_control(void)
{
    _asm {
        cmp eax, Sys_Dynamic_Device_Init
        jnz ctl_ok
        db  0Fh, 0Bh            /* ud2 */
      ctl_ok:
        clc
        ret
    }
}
