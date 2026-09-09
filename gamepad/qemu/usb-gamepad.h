/*
 * usb-gamepad: the host's gamepad as a USB HID device (M13 path A,
 * docs/tracks/m13-gamepads.md).
 *
 * The one header shared by the device (hw/usb/dev-gamepad.c) and whoever
 * drives it — today the embed shim (embed/libqemu_embed.c), through
 * qemu_embed_pad_*.
 *
 * The interface is deliberately **absolute state, not events**. The host
 * side always knows the whole pad — the player keeps it in `Pads` — so
 * sending all of it costs six bytes and cannot desync: a dropped update
 * is corrected by the next one, where a dropped *event* would leave the
 * guest holding a button forever. It is also what the HID report is
 * anyway, so nothing has to be accumulated on this side.
 */
#ifndef QEMU_USB_GAMEPAD_H
#define QEMU_USB_GAMEPAD_H

#include <stdbool.h>
#include <stdint.h>

/* X, Y, Z, Rz — the two sticks. 0x00..0xff, centre 0x80. */
#define USB_GAMEPAD_AXES 4
#define USB_GAMEPAD_BUTTONS 12
/* The hat's "not pressed" position. 0..7 are N, NE, E, ... NW. */
#define USB_GAMEPAD_HAT_NULL 8

/*
 * Replace the pad's state. Call with the BQL held (the embed shim's
 * input bottom half does). A no-op when no usb-gamepad is on the
 * machine, so a caller need not check first.
 */
void usb_gamepad_set_state(const uint8_t axes[USB_GAMEPAD_AXES],
                           uint8_t hat, uint16_t buttons);

/* Whether this machine has one at all. */
bool usb_gamepad_present(void);

#endif /* QEMU_USB_GAMEPAD_H */
