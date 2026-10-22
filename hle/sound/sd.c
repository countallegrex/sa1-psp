/* sd.c - the Katana sound driver interface (sd, 0x8C643400-0x8C643FFF).
 *
 * The game loads the Manatee driver (MASARU.DRV) and banks into guest RAM,
 * describes each with a memory block, and has the driver copy them into
 * the AICA's sound RAM: a whole multi-unit file (S-MLT: program banks SMPB,
 * sequence banks SMSB, DSP programs, each at its fixed sound-RAM address)
 * or one bank.  It then plays every effect and character voice clip as MIDI
 * on two direct ports, 16 channels each: bank select (CC 32 = the MLT bank
 * number), program change, pan, pitch bend, note on, volume ramps by CC 7
 * (docs/formats/sound.md section 4).  Here the copy goes into our own sound
 * RAM image and the MIDI bytes into the audio core (snd_core.c), which
 * plays the programs the way the driver and the AICA do.  The driver
 * binary itself is never run.
 *
 * Handles are small tagged numbers: the game only stores them and passes
 * them back.  Names are behavioural (docs/seam.md has no SDK names here).
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "../seam.h"
#include "../state.h"
#include "snd_host.h"
#include <string.h>
#include <stdlib.h>

#define MEMBLK_TAG 0x5D000000u
#define PORT_TAG   0x5D000100u
#define MAX_MEMBLK 8
#define MAX_PORTS 8
#define MAX_UNITS 32

static struct { uint32_t used, addr, size; } memblk[MAX_MEMBLK];
static uint32_t ports_open;
/* The unit table of the last multi-unit file (every MLT on the disc has the
 * same one): type tag, bank number, sound-RAM address and reserved size. */
static struct { uint32_t tag, bank, aica, reserve; } units[MAX_UNITS];
static uint32_t nunits;
static uint32_t downloads, notes;

static int log_on = -1;
static int snd_log(void) { if (log_on < 0) log_on = getenv("SA1_SND_LOG") != NULL; return log_on; }

static int memblk_index(uint32_t h) {
    uint32_t i = h - MEMBLK_TAG - 1;
    return (h & 0xFFFFFF00u) == MEMBLK_TAG && i < MAX_MEMBLK && memblk[i].used ? (int)i : -1;
}

/* Copies n guest bytes at src into sound RAM at dst and tells the core. */
static void to_sound_ram(uint32_t dst, uint32_t src, uint32_t n) {
    snd_shared *sh = snd_host_shared();
    if (!sh || dst >= SND_RAM_SIZE) return;
    if (n > SND_RAM_SIZE - dst) n = SND_RAM_SIZE - dst;
    for (uint32_t i = 0; i < n; i++) sh->sound_ram[dst + i] = (uint8_t)hle_rd8(src + i);
    snd_host_cmd(SND_CMD_BANK_CHANGED, 0, 0, 0, (int32_t)dst, (int32_t)n);
}

static void note_bank(uint32_t tag, uint32_t bank, uint32_t aica, uint32_t size) {
    snd_shared *sh = snd_host_shared();
    if (tag == 0x42504D53u && bank < 16 && sh) { sh->bank_addr[bank] = aica; sh->bank_size[bank] = size; }  /* "SMPB" */
}

/* 0x8C643F6C: driver/system init (0, 0, 0). */
void sdk_8c643f6c(sh4_ctx *c) { HLE_ENTER(0x8c643f6cu, "sdSysInit"); RET(0); }
/* 0x8C6436D0: (1) after init: a mode switch; nothing to do. */
void sdk_8c6436d0(sh4_ctx *c) { HLE_ENTER(0x8c6436d0u, "sdSysSetMode"); RET(0); }
/* 0x8C643E6C: (memblk) download the driver binary (MASARU.DRV) to the ARM7: not run. */
void sdk_8c643e6c(sh4_ctx *c) { HLE_ENTER(0x8c643e6cu, "sdDrvDownload"); RET(0); }
/* 0x8C64387C: output mode 'STRO' / 'MONO' (syCfg sound mode). */
void sdk_8c64387c(sh4_ctx *c) { HLE_ENTER(0x8c64387cu, "sdSysSetOutputMode"); RET(0); }
/* 0x8C6437F0: (0, 0) before the volume set-up at each bank load. */
void sdk_8c6437f0(sh4_ctx *c) { HLE_ENTER(0x8c6437f0u, "sdSysReset?"); RET(0); }
/* 0x8C643F98: the driver's per-frame server (the game calls it from its vsync routine). */
void sdk_8c643f98(sh4_ctx *c) { HLE_ENTER(0x8c643f98u, "sdSysServer"); RET(0); }
/* 0x8C6436E8: (a, b, c, 4) after each bank load: a status query the game
 * accepts 0 from. */
void sdk_8c6436e8(sh4_ctx *c) { HLE_ENTER(0x8c6436e8u, "sdSysGetInfo?"); RET(0); }

/* 0x8C6434E0: sdMemBlkCreate(&h) -> 0 and a handle. */
void sdk_8c6434e0(sh4_ctx *c) {
    HLE_ENTER(0x8c6434e0u, "sdMemBlkCreate");
    for (uint32_t i = 0; i < MAX_MEMBLK; i++)
        if (!memblk[i].used) { memblk[i].used = 1; memblk[i].addr = memblk[i].size = 0; hle_wr32(ARG0, MEMBLK_TAG + 1 + i); RET(0); return; }
    hle_wr32(ARG0, 0);
    RET(-1);
}
/* 0x8C6435BC: sdMemBlkDestroy(h). */
void sdk_8c6435bc(sh4_ctx *c) {
    HLE_ENTER(0x8c6435bcu, "sdMemBlkDestroy");
    int i = memblk_index(ARG0);
    if (i >= 0) memblk[i].used = 0;
    RET(0);
}
/* 0x8C643624: sdMemBlkSetPrm(h, addr, size, -1 | 0, 0): size 0 for a whole
 * multi-unit file. */
void sdk_8c643624(sh4_ctx *c) {
    HLE_ENTER(0x8c643624u, "sdMemBlkSetPrm");
    int i = memblk_index(ARG0);
    if (i >= 0) { memblk[i].addr = ARG1; memblk[i].size = ARG2; }
    RET(0);
}

/* 0x8C643970: sdMultiUnitDownload(h): every unit of the S-MLT file that has
 * data goes to its sound-RAM address; units without data keep what an
 * earlier file put there (a level file fills banks 0-2, 4, 5; the player
 * and voice files fill 3 and 6). */
void sdk_8c643970(sh4_ctx *c) {
    HLE_ENTER(0x8c643970u, "sdMultiUnitDownload");
    int i = memblk_index(ARG0);
    if (i < 0) { RET(-1); return; }
    uint32_t a = memblk[i].addr;
    if (hle_rd32(a) != 0x544C4D53u) { sa1_log("snd: multi-unit download of 0x%08x: no S-MLT header", a); RET(-1); return; }   /* "S" "MLT" */
    uint32_t n = hle_rd32(a + 8);
    if (n > MAX_UNITS) n = MAX_UNITS;
    nunits = n;
    uint32_t loaded = 0;
    for (uint32_t u = 0; u < n; u++) {
        uint32_t e = a + 0x20 + 0x20 * u;
        units[u].tag = hle_rd32(e); units[u].bank = hle_rd32(e + 4);
        units[u].aica = hle_rd32(e + 8); units[u].reserve = hle_rd32(e + 12);
        uint32_t off = hle_rd32(e + 16), size = hle_rd32(e + 20);
        if (off == 0xFFFFFFFFu || size == 0xFFFFFFFFu || !size) continue;
        to_sound_ram(units[u].aica, a + off, size);
        note_bank(units[u].tag, units[u].bank, units[u].aica, size);
        loaded++;
    }
    downloads++;
    if (snd_log()) sa1_log("snd: multi-unit download at 0x%08x: %u of %u units", a, loaded, n);
    RET(0);
}

/* 0x8C643904: sdBankDownload(h, type, bank): one bank (tag 'SMPB', 'SMSB',
 * 'SOSB') to the address the unit table gives it. */
void sdk_8c643904(sh4_ctx *c) {
    HLE_ENTER(0x8c643904u, "sdBankDownload");
    int i = memblk_index(ARG0);
    if (i < 0) { RET(-1); return; }
    for (uint32_t u = 0; u < nunits; u++)
        if (units[u].tag == ARG1 && units[u].bank == ARG2) {
            uint32_t size = memblk[i].size;
            if (size > units[u].reserve) size = units[u].reserve;
            to_sound_ram(units[u].aica, memblk[i].addr, size);
            note_bank(ARG1, ARG2, units[u].aica, size);
            RET(0);
            return;
        }
    sa1_log("snd: bank download of %.4s %u: no unit for it", (const char *)&c->r[5], ARG2);
    RET(-1);
}

/* 0x8C643AC0: sdPortOpen(&port): the game opens eight, in order; the first
 * two are the MIDI direct ports (channels 0-15 and 16-31), the others the
 * sequence ports of its "channels" 32-37. */
void sdk_8c643ac0(sh4_ctx *c) {
    HLE_ENTER(0x8c643ac0u, "sdPortOpen");
    hle_wr32(ARG0, PORT_TAG + (ports_open % MAX_PORTS));
    ports_open++;
    RET(0);
}

static int port_index(uint32_t h) { return (h & 0xFFFFFF00u) == PORT_TAG ? (int)(h & 0xFF) : -1; }

/* 0x8C643C18: sdMidiMakeMessage(port, msg, status, d1, d2): variadic, the
 * bytes on the stack; writes them into the game's 12-byte message buffer. */
void sdk_8c643c18(sh4_ctx *c) {
    HLE_ENTER(0x8c643c18u, "sdMidiMakeMessage");
    uint32_t st = STACK_ARG(0) & 0xFF;
    uint32_t len = (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 2 : 3;
    hle_wr8(ARG1, st);
    hle_wr8(ARG1 + 1, STACK_ARG(1) & 0x7F);
    hle_wr8(ARG1 + 2, len == 3 ? (STACK_ARG(2) & 0x7F) : 0);
    hle_wr8(ARG1 + 3, len);
    RET(0);
}

/* 0x8C643B9C: sdMidiSendMessage(port, msg, 0). */
void sdk_8c643b9c(sh4_ctx *c) {
    HLE_ENTER(0x8c643b9cu, "sdMidiSendMessage");
    int p = port_index(ARG0);
    uint32_t st = hle_rd8(ARG1), d1 = hle_rd8(ARG1 + 1), d2 = hle_rd8(ARG1 + 2);
    if (p >= 0 && p < SND_PORTS) {
        snd_host_cmd(SND_CMD_MIDI, (uint8_t)p, (uint8_t)st, (uint8_t)d1, (int32_t)d2, 0);
        if ((st & 0xF0) == 0x90 && d2) notes++;
    }
    if (snd_log()) sa1_log("snd midi %d %02x %02x %02x", p, st, d1, d2);
    RET(0);
}

/* Sequence ports (the game's channels 32-37).  The route plays no sequence
 * (two start calls with all-zero arguments at the Chaos 0 result and the
 * Emerald Coast goal), so these are logged and accepted. */
void sdk_8c643b08(sh4_ctx *c) {
    HLE_ENTER(0x8c643b08u, "sdMidiPlay");
    if (snd_log()) sa1_log("snd seq play port %d %08x %08x %08x", port_index(ARG0), ARG1, ARG2, ARG3);
    RET(0);
}
void sdk_8c643e08(sh4_ctx *c) { HLE_ENTER(0x8c643e08u, "sdMidiStop"); RET(0); }
/* 0x8C643D88: sdMidiSetVol(port, level, fade) is driver command 5: the port's
 * level offset fades to level.  The game calls it only for the sequence
 * ports, so the MIDI ports keep offset 0 (snd_core's level law). */
void sdk_8c643d88(sh4_ctx *c) { HLE_ENTER(0x8c643d88u, "sdMidiSetVol"); RET(0); }

/* 0x8C643754: sdSysSetVolume(out, pan, level) is driver command 0x83 (MASARU.DRV
 * 0x5b88): it sets EFSDL/EFPAN of the AICA DSP output mixer slot `out`
 * (0x802000 + 4 * out), the driver's default plus level / 8 steps of 3 dB.
 * It never touches a tone's TL or DISDL, so with no DSP here it has nothing
 * to do.  The game sets outs 0 and 1 to -48 and 2, 3 to 0 at every bank load. */
void sdk_8c643754(sh4_ctx *c) {
    HLE_ENTER(0x8c643754u, "sdSysSetVolume");
    if (snd_log()) sa1_log("snd dsp out %u pan %d level %d", (unsigned)(ARG0 & 0xF), (int8_t)ARG1, (int8_t)ARG2);
    RET(0);
}

void snd_sd_state(hle_state_io *io) {
    hle_state_blob(io, "sd.memblk", memblk, sizeof memblk);
    hle_state_blob(io, "sd.ports", &ports_open, sizeof ports_open);
    hle_state_blob(io, "sd.units", units, sizeof units);
    hle_state_blob(io, "sd.nunits", &nunits, sizeof nunits);
}

void snd_sd_report(void) { sa1_log("snd: %u bank downloads, %u notes played", downloads, notes); }
