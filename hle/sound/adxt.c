/* adxt.c - CRI ADX streaming (ADXT, 0x8C644000-0x8C645FFF, and the ADXF
 * file queries at 0x8C65C25C-0x8C65C2A8).
 *
 * The game keeps two players (its ADX manager at 0x8C107864): a stereo one
 * for music, started by file name from the table at 0x8C5FB8A8 (S_SQUARE.ADX,
 * ECOAST1.ADX, ...), and a mono one for voice lines, started by index in
 * partition 0, the AFS archive the manager loaded with ADXF (event_adx.afs
 * or event_adx_us.afs, chosen by the voice-language word 0x8C754C00).  It
 * polls the status every frame and cutscenes wait for a voice line to end
 * (status 0 or 5) before they go on (docs/formats/sound.md section 2).
 *
 * A player is a 116-byte record in the SDK's own table at 0x8C6737B0 (the
 * game only passes it back; the status byte at +1 is kept current as the
 * library keeps it).  Streams are decoded by the audio core; this file maps
 * the calls onto snd_host.
 *
 * Part of sa1-psp (MIT). */
#include "../hle.h"
#include "../seam.h"
#include "../state.h"
#include "../shinobi/shinobi.h"
#include "snd_host.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ADXT_TABLE 0x8C6737B0u
#define ADXT_REC 116u
#define ADXT_MAX 8
#define VOICE_LANG_WORD 0x8C754C00u   /* 0 Japanese voices, 1 English (set at boot from the system language) */

static struct { uint32_t used, stream, maxch; } adxt[ADXT_MAX];
static char partition[4][96];       /* ADXF partitions by id: data-root path of the AFS */
static char last_query[96];         /* the file the CRI layer last asked about */
static uint32_t starts_bgm, starts_voice, missing;

static int log_on = -1;
static int snd_log(void) { if (log_on < 0) log_on = getenv("SA1_SND_LOG") != NULL; return log_on; }

static int handle_index(uint32_t h) {
    if (h < ADXT_TABLE || h >= ADXT_TABLE + ADXT_MAX * ADXT_REC || (h - ADXT_TABLE) % ADXT_REC) return -1;
    int i = (int)((h - ADXT_TABLE) / ADXT_REC);
    return adxt[i].used ? i : -1;
}

/* The status the guest sees (snd_guest_timing: none = always stopped). */
static uint32_t guest_state(int i) { return snd_guest_timing ? snd_stream_state((int)adxt[i].stream) : 0; }

/* The status byte the library keeps at +1. */
static void sync_status(int i) { hle_wr8(ADXT_TABLE + (uint32_t)i * ADXT_REC + 1, guest_state(i)); }

/* Data-root path of a name the game gives (relative to the gdFs directory,
 * as the CRI layer resolves it); falls back to SONICADV/. */
static void resolve(char *out, unsigned n, const char *name) {
    shinobi_gdfs_path(out, n, name);
    uint32_t size;
    if (sa1_plat_file_size(out, &size) == 0) return;
    char up[96];
    snprintf(up, sizeof up, "%s", name);
    for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    snprintf(out, n, "SONICADV/%s", up);
}

/* ---- ADXF ------------------------------------------------------------------- */
/* 0x8C65C25C (fname, &sector) / 0x8C65C27A / 0x8C65C2A8: where a file is on
 * the disc.  The answer stays -1, so the CRI partition loader (recompiled,
 * 0x8C10C1C8) gives up before it reads anything, as it did before audio;
 * the file name is what we need from it: ADXF_LoadPartitionNw(0, name, ...)
 * asks this first, and partition 0 is the only one the game loads. */
void sdk_8c65c25c(sh4_ctx *c) {
    HLE_ENTER(0x8c65c25cu, "ADXF file start");
    char name[96];
    hle_guest_string(ARG0, name, sizeof name);
    resolve(last_query, sizeof last_query, name);
    snprintf(partition[0], sizeof partition[0], "%s", last_query);
    if (snd_log()) sa1_log("snd: ADXF query \"%s\" -> %s", name, last_query);
    RET(-1);
}
void sdk_8c65c27a(sh4_ctx *c) { HLE_ENTER(0x8c65c27au, "ADXF file sectors"); RET(-1); }
void sdk_8c65c2a8(sh4_ctx *c) { HLE_ENTER(0x8c65c2a8u, "ADXF file bytes"); RET(-1); }

/* ---- ADXT ------------------------------------------------------------------- */
/* 0x8C644D84, 0x8C64401C, 0x8C644022, 0x8C644DE4: library set-up at boot
 * and at each ADX manager start; 0x8C64489C (func, 0): the error callback.
 * Nothing to keep. */
void sdk_8c644d84(sh4_ctx *c) { HLE_ENTER(0x8c644d84u, "ADXT_Init"); RET(0); }
void sdk_8c64401c(sh4_ctx *c) { HLE_ENTER(0x8c64401cu, "ADXT_Init2"); RET(0); }
void sdk_8c644022(sh4_ctx *c) { HLE_ENTER(0x8c644022u, "ADXT_Init3"); RET(0); }
void sdk_8c644de4(sh4_ctx *c) { HLE_ENTER(0x8c644de4u, "ADXT_Finish?"); RET(0); }
void sdk_8c64489c(sh4_ctx *c) { HLE_ENTER(0x8c64489cu, "ADXT_EntryErrFunc"); RET(0); }

/* 0x8C6446E2: ADXT_ExecServer, once per frame from the game's vsync work:
 * the host runs the core at the frame boundary, so only the status bytes. */
void sdk_8c6446e2(sh4_ctx *c) {
    HLE_ENTER(0x8c6446e2u, "ADXT_ExecServer");
    for (int i = 0; i < ADXT_MAX; i++) if (adxt[i].used) sync_status(i);
    if (snd_voice_lang >= 0 && hle_rd32(VOICE_LANG_WORD) != (uint32_t)snd_voice_lang) hle_wr32(VOICE_LANG_WORD, (uint32_t)snd_voice_lang);
    RET(0);
}

/* 0x8C64413C: ADXT_Create(max channels, work, work size) -> player or NULL.
 * The stereo player plays music (stream 0), the mono one voices (1). */
void sdk_8c64413c(sh4_ctx *c) {
    HLE_ENTER(0x8c64413cu, "ADXT_Create");
    for (int i = 0; i < ADXT_MAX; i++) {
        if (adxt[i].used) continue;
        uint32_t h = ADXT_TABLE + (uint32_t)i * ADXT_REC;
        for (uint32_t k = 0; k < ADXT_REC; k += 4) hle_wr32(h + k, 0);
        hle_wr8(h, 1);
        adxt[i].used = 1; adxt[i].maxch = ARG0; adxt[i].stream = ARG0 >= 2 ? 0 : 1;
        RET(h);
        return;
    }
    RET(0);
}

/* 0x8C6442F2: ADXT_Destroy(player). */
void sdk_8c6442f2(sh4_ctx *c) {
    HLE_ENTER(0x8c6442f2u, "ADXT_Destroy");
    int i = handle_index(ARG0);
    if (i >= 0) { snd_stream_stop((int)adxt[i].stream); adxt[i].used = 0; hle_wr8(ARG0, 0); }
    RET(0);
}

/* 0x8C644968: ADXT_StartFname(player, name). */
void sdk_8c644968(sh4_ctx *c) {
    HLE_ENTER(0x8c644968u, "ADXT_StartFname");
    int i = handle_index(ARG0);
    if (i < 0) { RET(0); return; }
    char name[96], path[128];
    hle_guest_string(ARG1, name, sizeof name);
    resolve(path, sizeof path, name);
    if (snd_stream_start((int)adxt[i].stream, path, 0, 0) != 0) missing++;
    else starts_bgm++;
    if (snd_log()) sa1_log("snd: ADXT %d start \"%s\"", i, path);
    sync_status(i);
    RET(0);
}

/* 0x8C644904: ADXT_StartAfs(player, partition, file index). */
void sdk_8c644904(sh4_ctx *c) {
    HLE_ENTER(0x8c644904u, "ADXT_StartAfs");
    int i = handle_index(ARG0);
    if (i < 0 || ARG1 >= 4 || !partition[ARG1][0]) { if (i >= 0) sync_status(i); RET(0); return; }
    const char *afs = partition[ARG1];
    uint8_t toc[8];
    uint32_t fid = ARG2;
    if (sa1_plat_file_read(afs, 0, toc, 8) != 8 || memcmp(toc, "AFS", 3) != 0) { missing++; RET(0); return; }
    uint32_t count = toc[4] | toc[5] << 8 | (uint32_t)toc[6] << 16 | (uint32_t)toc[7] << 24;
    uint8_t e[8];
    if (fid >= count || sa1_plat_file_read(afs, 8 + 8 * fid, e, 8) != 8) { missing++; RET(0); return; }
    uint32_t off = e[0] | e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24;
    uint32_t size = e[4] | e[5] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24;
    if (snd_stream_start((int)adxt[i].stream, afs, off, size) != 0) missing++;
    else starts_voice++;
    if (snd_log()) sa1_log("snd: ADXT %d start %s #%u (+0x%x, %u bytes)", i, afs, fid, off, size);
    sync_status(i);
    RET(0);
}

/* 0x8C644468: ADXT_Stop(player). */
void sdk_8c644468(sh4_ctx *c) {
    HLE_ENTER(0x8c644468u, "ADXT_Stop");
    int i = handle_index(ARG0);
    if (i >= 0) { snd_stream_stop((int)adxt[i].stream); sync_status(i); }
    RET(0);
}

/* 0x8C644500: ADXT_GetStat(player) -> 0 stopped, 3 playing, 5 play end. */
void sdk_8c644500(sh4_ctx *c) {
    HLE_ENTER(0x8c644500u, "ADXT_GetStat");
    int i = handle_index(ARG0);
    RET(i >= 0 ? guest_state(i) : 0);
}

/* 0x8C644504: ADXT_GetTime(player, &count, &rate): samples played and the
 * sample rate (time = count / rate seconds); 0 and 1 when not playing. */
void sdk_8c644504(sh4_ctx *c) {
    HLE_ENTER(0x8c644504u, "ADXT_GetTime");
    int i = handle_index(ARG0);
    uint32_t n = 0, r = 1;
    if (i >= 0 && snd_guest_timing) snd_stream_time((int)adxt[i].stream, &n, &r);
    hle_wr32(ARG1, n); hle_wr32(ARG2, r);
    RET(0);
}

/* 0x8C6445EA: ADXT_SetOutVol(player, level in 0.1 dB): -10 for music, 0 for
 * voices, -960 (silence) before a fade-in; stored at +64 as the library does. */
void sdk_8c6445ea(sh4_ctx *c) {
    HLE_ENTER(0x8c6445eau, "ADXT_SetOutVol");
    int i = handle_index(ARG0);
    if (i >= 0) { hle_wr16(ARG0 + 64, ARG1); snd_stream_volume((int)adxt[i].stream, (int16_t)ARG1); }
    RET(0);
}

/* 0x8C6446D2 (player, 2 | 1) and 0x8C644602 (player, 20): stores into the
 * record (+109 byte, +56 word) the game never reads back. */
void sdk_8c6446d2(sh4_ctx *c) { HLE_ENTER(0x8c6446d2u, "ADXT_SetOutputChannels?"); if (handle_index(ARG0) >= 0) hle_wr8(ARG0 + 109, ARG1); RET(0); }
void sdk_8c644602(sh4_ctx *c) { HLE_ENTER(0x8c644602u, "ADXT_SetReloadTime?"); if (handle_index(ARG0) >= 0) hle_wr32(ARG0 + 56, ARG1); RET(0); }

void snd_adxt_state(hle_state_io *io) {
    hle_state_blob(io, "adxt.players", adxt, sizeof adxt);
    hle_state_blob(io, "adxt.part", partition, sizeof partition);
}

void snd_adxt_report(void) {
    sa1_log("snd: %u music starts, %u voice starts, %u missing", starts_bgm, starts_voice, missing);
}
