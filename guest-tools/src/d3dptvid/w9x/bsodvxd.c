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

#ifdef BSOD_TIMER
/* bsodtmr.vxd: the same fault from a timer callback a second after load
 * instead of from init. A fault in a VxD's init reaches the adapter through
 * the VDD's screen switch (PRE_HIRES_TO_VGA); one outside any VM's own
 * execution -- the patch-44 corruption's, from VTDAPI's timer event
 * (2026-09-12) -- gets its blue screen with no switch and no mini-VDD call
 * at all, only the VMM's Begin_Message_Mode, and that is the one this
 * trigger makes. Two things keep the VxD loaded until the callback runs:
 * W32_DEVICEIOCONTROL answers DIOC_OPEN with EAX = 0 (otherwise CreateFile
 * fails and the loader unloads the VxD, pending time-out and all), and
 * bsod.exe holds the handle open. */
static void __declspec(naked) fault_cb(void)
{
    _asm {
        db  0Fh, 0Bh            /* ud2 */
        ret
    }
}

static void __declspec(naked) arm_timer(void)
{
    _asm {
        push esi
        mov  eax, 1000          /* ms */
        xor  edx, edx           /* reference data */
        mov  esi, offset fault_cb
    }
    VMMCall(Set_Global_Time_Out);
    _asm {
        pop  esi
        ret
    }
}

void __declspec(naked) VXD_control(void)
{
    _asm {
        cmp eax, Sys_Dynamic_Device_Init
        jnz ctl_ioctl
        call arm_timer
        jmp ctl_ok
      ctl_ioctl:
        cmp eax, W32_DEVICEIOCONTROL
        jnz ctl_ok
        xor eax, eax            /* DIOC_OPEN and the rest: succeed */
      ctl_ok:
        clc
        ret
    }
}
#else
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
#endif
