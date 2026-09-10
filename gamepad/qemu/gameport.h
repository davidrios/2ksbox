/*
 * gameport: the PC analog joystick port at 0x201 (M13 path B,
 * docs/tracks/m13-gamepads.md).
 *
 * The one header shared by the device (hw/input/gameport.c) and whoever
 * drives it — the embed shim (embed/libqemu_embed.c), which feeds this
 * and the usb-gamepad from the same qemu_embed_pad_state call. A machine
 * has one or the other (bundle::Pad is a single choice), and the shim
 * does not have to know which: both entry points are no-ops when their
 * device is absent.
 *
 * Absolute state rather than events, for the reason usb-gamepad.h gives:
 * the host always knows the whole pad, so a dropped update is corrected
 * by the next one where a dropped *event* would leave a button held.
 */
#ifndef QEMU_GAMEPORT_H
#define QEMU_GAMEPORT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Four one-shots and four buttons — the hardware's own limit, not ours:
 * a connector carries two axes and two buttons, and four of each is what
 * a Y-cable or a four-axis card gives. There is nowhere on this port for
 * a hat, a third stick or buttons 5-12, which is why path A exists.
 */
#define GAMEPORT_AXES 4
#define GAMEPORT_BUTTONS 4

/*
 * Replace the pad's state. Call with the BQL held (the embed shim's
 * input bottom half does). The arguments are deliberately the *same*
 * ones usb_gamepad_set_state() takes, so one call in the shim feeds
 * either device: `axes` is X, Y, Z, Rz — the two sticks, 0x00..0xff with
 * 0x80 centred — `hat` is 0..7 clockwise from north or 8 for released,
 * and `buttons` is the twelve-button bitmap of which this port can carry
 * the first four. A no-op when no gameport is on the machine.
 */
void gameport_set_state(const uint8_t axes[GAMEPORT_AXES], uint8_t hat,
                        uint16_t buttons);

/* Whether this machine has one at all. */
bool gameport_present(void);

#endif /* QEMU_GAMEPORT_H */
