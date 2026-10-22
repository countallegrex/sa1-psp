/* snd_core.h - the audio core: everything that turns commands into samples.
 *
 * What the console split between the AICA (64 tone channels reading 2 MB of
 * sound RAM), the Manatee driver on the ARM7 (MIDI channels, programs from
 * the downloaded banks) and the SH-4 side of CRI ADX (stream decoding) is
 * one self-contained module here, so that it can run where there is a spare
 * core: on the caller's thread on the desktop and in PPSSPP, on the PSP's
 * Media Engine later (README section 6).  To make that possible it
 *
 *  - touches only memory it is handed: the sound RAM image, its own state
 *    struct, and the rings and status block in snd_shared;
 *  - talks to the guest side only through those: a command ring (MIDI
 *    bytes, stream start/stop/volume), one compressed-data ring per stream
 *    (whole ADX block groups, filled by the main side from the files), and
 *    a status block it writes (stream state, samples played);
 *  - uses integer arithmetic per sample and no library calls after
 *    snd_core_init, allocates nothing, and keeps no pointers into guest RAM.
 *
 * The main side (hle/sound/snd_host.c) owns the files and the guest; the
 * core owns time.  Time is counted in output samples only: the host calls
 * snd_core_render with SND_FRAME_SAMPLES per guest frame, so a run under
 * --strict renders the same samples on any machine.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>
#include "adx.h"

#define SND_RATE 44100
#define SND_FRAME_SAMPLES 735           /* 44100 / 60: one guest frame */
#define SND_RAM_SIZE (2u << 20)         /* the AICA's 2 MB */
#define SND_VOICES 64                   /* tone channels, as on the AICA */
#define SND_PORTS 2                     /* MIDI direct ports (16 channels each) */
#define SND_STREAMS 2                   /* ADX players: 0 BGM (stereo), 1 voice (mono) */
#define SND_STREAM_RING (64u * 1024u)   /* compressed bytes per stream, a power of two */
#define SND_CMD_RING 1024               /* commands, a power of two */

/* ---- single-producer single-consumer rings ---------------------------------
 * head is written only by the producer, tail only by the consumer.  On the
 * PSP the two sides are different cores without cache coherence: the ring
 * memory must then be uncached (or written back/invalidated around each
 * access) and SND_BARRIER a sync; on one core it is a compiler barrier. */
#ifndef SND_BARRIER
#define SND_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

typedef struct snd_cmd {
    uint8_t op, a, b, c;                /* op and three byte arguments */
    int32_t x, y;                       /* wider arguments */
} snd_cmd;

enum { SND_MUTE_FX = 1, SND_MUTE_MUSIC = 2, SND_MUTE_VOICE = 4 };

enum {
    SND_CMD_MIDI = 1,                   /* a = port, b..c and x&0xFF: status, d1, d2 */
    SND_CMD_MIDI_RESET,                 /* every voice off, channels to defaults */
    SND_CMD_UNUSED3,                    /* was the group level (sdSysSetVolume); slot kept */
    SND_CMD_STREAM_START,               /* a = stream, x = ring position of its first byte, y = generation; info in snd_shared */
    SND_CMD_STREAM_STOP,                /* a = stream */
    SND_CMD_STREAM_VOL,                 /* a = stream, x = level in 0.1 dB (0 = unity, -960 = off) */
    SND_CMD_BANK_CHANGED,               /* sound RAM rewritten: stop the voices reading x..x+y */
};

typedef struct snd_cmd_ring {
    volatile uint32_t head, tail;
    snd_cmd cmd[SND_CMD_RING];
} snd_cmd_ring;

typedef struct snd_byte_ring {
    volatile uint32_t head, tail;       /* byte counts, free-running */
    uint8_t buf[SND_STREAM_RING];
} snd_byte_ring;

/* What the core reports back, per stream.  state uses the ADXT codes the
 * game reads: 0 stopped, 3 playing, 5 play end. */
typedef struct snd_stream_status {
    volatile uint32_t state;
    volatile uint32_t played;           /* output-rate samples since the start */
    volatile uint32_t pos;              /* source samples decoded since the start (loops included) */
    volatile uint32_t generation;       /* the start command it answers */
} snd_stream_status;

/* Everything the two sides share. */
typedef struct snd_shared {
    snd_cmd_ring cmds;                          /* main -> core */
    snd_byte_ring stream_data[SND_STREAMS];     /* main -> core: block groups */
    adx_info stream_info[SND_STREAMS];          /* written before STREAM_START */
    snd_stream_status status[SND_STREAMS];      /* core -> main */
    volatile uint32_t voices_active;            /* core -> main: tone voices sounding */
    volatile uint32_t rendered;                 /* core -> main: samples rendered in total */
    /* main -> core: where each program bank (SMPB, by MIDI bank number)
     * sits in sound RAM, from the multi-unit table; size 0 = not loaded. */
    volatile uint32_t bank_addr[16], bank_size[16];
    /* main -> core, debug: SND_MUTE_* bits of what not to mix (stems) */
    volatile uint32_t mute;
    /* debug: when nonzero, only effect voices of program (solo & 0xFF) in
     * bank (solo >> 8 & 0xFF) are mixed (bit 16 set marks it valid) */
    volatile uint32_t solo;
    uint8_t *sound_ram;                         /* SND_RAM_SIZE bytes, written by the main side */
} snd_shared;

/* ---- the core ------------------------------------------------------------ */
typedef struct snd_core snd_core;
uint32_t snd_core_size(void);                   /* bytes of the opaque state */
void snd_core_init(snd_core *core, snd_shared *sh);
/* Consumes pending commands, then mixes n stereo samples (interleaved
 * left/right, signed 16-bit) into out. */
void snd_core_render(snd_core *core, int16_t *out, uint32_t n);
/* Save states: the core's state is plain data apart from its pointer to
 * the shared block, which init sets and a load must keep. */
void snd_core_rebind(snd_core *core, snd_shared *sh);

/* ---- producer helpers (main side) --------------------------------------- */
int snd_cmd_push(snd_shared *sh, const snd_cmd *c);        /* 0, or -1 when full */
/* Stream data: the producer appends whole block groups in play order (the
 * loop rule: groups 0 .. G-1, then loop_start/32 .. G-1 again and again,
 * G = adx_play_groups); a STREAM_START names the ring position of the
 * stream's first byte, so stale bytes of the previous stream are skipped
 * without the producer ever moving the consumer's tail. */
uint32_t snd_ring_free(const snd_byte_ring *r);
void snd_ring_write(snd_byte_ring *r, const uint8_t *p, uint32_t n);   /* n <= snd_ring_free */
