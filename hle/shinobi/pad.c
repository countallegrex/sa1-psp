/* pad.c - peripherals (pd): the controller records the game reads every
 * frame.  24 records of 52 bytes at 0x8C88E9A4 (4 ports x 6 units; A0 is
 * index 0).  Record layout (confirmed from the SDK server's stores):
 *   +0 function id (1 = controller, 0 = nothing connected), +4 support bits,
 *   +8 on (held, active-high), +12 off, +16 press, +20 release,
 *   +24/+26 u16 right/left trigger, +28/+30 s16 stick x/y (centred at 0),
 *   +32/+34 s16 second stick, +36 name pointer, +40 extension,
 *   +44 old (previous on), +48 device-info pointer.
 *
 * Input comes from the backend through hle_pad_state (a neutral pad by
 * default); the desktop build scripts button presses by frame so the boot
 * can walk the title screen without a controller.
 *
 * Part of sa1-psp (MIT). */
#include "shinobi.h"
#include "../state.h"

#define PAD_TABLE 0x8C88E9A4u
#define PAD_RECORD 52
#define PAD_UNITS 24

hle_pad hle_pad_state;      /* written by the backend before each frame */
static uint32_t prev_on;

#ifdef SA1_PAD_PULSE
/* Debug aid (desktop, -DSA1_PAD_PULSE): SA1_PAD_PULSE="START:END:PERIOD:
 * BUTTONS:HOLD[,...]" adds BUTTONS (hex) for HOLD frames every PERIOD
 * frames from START to END, on top of the backend's script.  Long fights
 * need more presses than the backend's --press table holds. */
#include <stdio.h>
#include <stdlib.h>
static uint32_t pulse_buttons(uint32_t frame) {
    const char *p = getenv("SA1_PAD_PULSE");
    uint32_t b = 0;
    while (p && *p) {
        unsigned s, e, per, btn, hold;
        int n = 0;
        if (sscanf(p, "%u:%u:%u:%x:%u%n", &s, &e, &per, &btn, &hold, &n) != 5 || !per) break;
        if (frame >= s && frame < e && (frame - s) % per < hold) b |= btn;
        p += n;
        if (*p == ',') p++;
    }
    return b;
}
#endif

void sdk_8c601a34(sh4_ctx *c) {
    HLE_ENTER(0x8c601a34u, "pdInitPeripheral");
    for (uint32_t a = PAD_TABLE; a < PAD_TABLE + PAD_UNITS * PAD_RECORD; a += 4) hle_wr32(a, 0);
    hle_wr32(0x8C88EEE4u, ARG0);           /* logic setting the SDK keeps */
    prev_on = 0;
}

/* 0x8C601874 / 0x8C601F5A: pdGetPeripheral(unit) -> the record's address. */
static void get_peripheral(sh4_ctx *c, uint32_t addr, const char *name) {
    HLE_ENTER(addr, name);
    uint32_t idx = ARG0 < PAD_UNITS ? ARG0 : 0;
    RET(PAD_TABLE + idx * PAD_RECORD);
}
void sdk_8c601874(sh4_ctx *c) { get_peripheral(c, 0x8c601874u, "pdGetPeripheral"); }
void sdk_8c601f5a(sh4_ctx *c) { get_peripheral(c, 0x8c601f5au, "pdGetPeripheral"); }

/* 0x8C601E2A: pdExecPeripheralServer: sample the pad into record 0. */
void sdk_8c601e2a(sh4_ctx *c) {
    HLE_ENTER(0x8c601e2au, "pdExecPeripheralServer");
    uint32_t r = PAD_TABLE;
    uint32_t on = hle_pad_state.buttons;
#ifdef SA1_PAD_PULSE
    on |= pulse_buttons(hle_frame_count());
#endif
    hle_wr32(r + 0, 0x01000000u);                    /* Maple function: controller, on A0 */
    hle_wr32(r + 4, 0x000CFE0Eu);                    /* support bits; the game tests 0x000C000E (0x8C050C2E) */
    hle_wr32(r + 8, on);
    hle_wr32(r + 12, ~on);
    hle_wr32(r + 16, on & ~prev_on);
    hle_wr32(r + 20, prev_on & ~on);
    hle_wr16(r + 24, hle_pad_state.rtrig);
    hle_wr16(r + 26, hle_pad_state.ltrig);
    hle_wr16(r + 28, (uint16_t)(int16_t)hle_pad_state.x);
    hle_wr16(r + 30, (uint16_t)(int16_t)hle_pad_state.y);
    hle_wr16(r + 32, 0);
    hle_wr16(r + 34, 0);
    hle_wr32(r + 44, prev_on);
    prev_on = on;
    for (int u = 1; u < PAD_UNITS; u++) hle_wr32(PAD_TABLE + (uint32_t)u * PAD_RECORD, 0);
}

void shinobi_pad_state(hle_state_io *io) {
    hle_state_blob(io, "pad.state", &hle_pad_state, sizeof hle_pad_state);
    hle_state_blob(io, "pad.prev", &prev_on, sizeof prev_on);
}
