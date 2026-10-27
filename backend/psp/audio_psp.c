/* audio_psp.c - backend/audio.h on the PSP: one sceAudio channel fed by its
 * own thread.
 *
 * The game's thread renders 735 frames per guest frame (hle/sound/snd_host.c)
 * and audio_push copies them into a ring; the output thread takes blocks of
 * AUD_BLOCK frames from the ring and hands each to sceAudioOutputBlocking,
 * which returns when the channel can take the next one, so the hardware
 * paces the thread.  The ring is single producer, single consumer: the game
 * thread moves only the write index, the output thread only the read index.
 * When the game is slower than real time the thread waits for a whole block
 * (the channel plays out what it has, then a gap; padding part-blocks with
 * silence made more, shorter gaps), and counts the waits.  When the game
 * runs ahead (an unpaced run, a catch-up after a load) a push beyond AUD_MAX
 * buffered frames is dropped, so the latency cannot drift.  The mixer
 * itself stays on the game's thread (the Media Engine is not used: the
 * toolchain has no ME loader).
 *
 * Part of sa1-psp (MIT). */
#include "../audio.h"
#include "hle.h"

#include <pspkernel.h>
#include <pspaudio.h>
#include <string.h>

#define AUD_BLOCK 1024u                 /* frames per sceAudio output (23 ms) */
#define AUD_RING 8192u                  /* frames; a power of two */
#define AUD_START 2048u                 /* frames buffered before the first output */
#define AUD_MAX 4096u                   /* frames (93 ms): beyond this a push is dropped */

int audio_psp_enabled = 1;              /* main_psp.c --no-audio */
int audio_psp_out2 = 1;                 /* sceAudioOutput2 (ec +0.35 ms, ss +0.22 a frame); main_psp.c --audio-chan: a sceAudio channel (ec +2.1, ss +1.9; 2026-10-27) */

static int16_t ring[2 * AUD_RING] __attribute__((aligned(64)));
static int16_t block[2][2 * AUD_BLOCK] __attribute__((aligned(64)));
static volatile uint32_t wr, rd;        /* frames, free-running */
static volatile int run;
static int ch = -1;
static SceUID th = -1, sema = -1;
static volatile int waiting;            /* the thread sleeps on sema until a push */
static uint32_t starves, dropped;
static uint64_t starve_us;

static int out_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int b = 0;
    while (wr - rd < AUD_START && run) { waiting = 1; sceKernelWaitSema(sema, 1, NULL); }
    while (run) {
        if (wr - rd < AUD_BLOCK) {
            const uint64_t t0 = sa1_plat_time_us();
            while (run && wr - rd < AUD_BLOCK) { waiting = 1; sceKernelWaitSema(sema, 1, NULL); }
            starves++; starve_us += sa1_plat_time_us() - t0;
            if (!run) break;
        }
        int16_t *o = block[b];
        for (uint32_t i = 0; i < AUD_BLOCK; ) {     /* at most two runs around the ring's end */
            const uint32_t at = (rd + i) & (AUD_RING - 1), k = AUD_RING - at < AUD_BLOCK - i ? AUD_RING - at : AUD_BLOCK - i;
            memcpy(o + 2 * i, ring + 2 * at, 4 * k);
            i += k;
        }
        rd += AUD_BLOCK;
        sceKernelDcacheWritebackRange(o, sizeof block[0]);
        if (audio_psp_out2) sceAudioOutput2OutputBlocking(PSP_AUDIO_VOLUME_MAX, o);
        else sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, o);
        b ^= 1;
    }
    return 0;
}

int audio_open(uint32_t rate, uint32_t channels) {
    if (!audio_psp_enabled || rate != 44100 || channels != 2) return -1;
    ch = audio_psp_out2 ? sceAudioOutput2Reserve(AUD_BLOCK) : sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AUD_BLOCK, PSP_AUDIO_FORMAT_STEREO);
    if (audio_psp_out2 && ch >= 0) ch = 0;
    if (ch < 0) { sa1_log("audio: sceAudioChReserve failed (%08x)", (unsigned)ch); return -1; }
    run = 1;
    sema = sceKernelCreateSema("sa1_audio", 0, 0, 1, NULL);
    /* above the game's thread (0x20), so a block is never late because the
     * game is busy; it sleeps in sceAudioOutputBlocking almost all the time */
    th = sceKernelCreateThread("sa1_audio", out_thread, 0x12, 0x4000, 0, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        sa1_log("audio: output thread failed (%08x)", (unsigned)th);
        if (audio_psp_out2) sceAudioOutput2Release(); else sceAudioChRelease(ch);
        ch = -1; run = 0;
        return -1;
    }
    sa1_log("audio: %s %d, %u-frame blocks, %u frames buffered before the start", audio_psp_out2 ? "sceAudioOutput2" : "sceAudio channel", ch, AUD_BLOCK, AUD_START);
    return 0;
}

void audio_push(const int16_t *samples, uint32_t frames) {
    if (ch < 0) return;
    if (wr - rd + frames > AUD_MAX) { dropped += frames; return; }
    for (uint32_t i = 0; i < frames; ) {
        const uint32_t at = (wr + i) & (AUD_RING - 1), k = AUD_RING - at < frames - i ? AUD_RING - at : frames - i;
        memcpy(ring + 2 * at, samples + 2 * i, 4 * k);
        i += k;
    }
    __asm__ volatile("sync" ::: "memory");   /* the samples before the index */
    wr += frames;
    if (waiting) { waiting = 0; sceKernelSignalSema(sema, 1); }   /* no polling: the thread waits for this */
}

uint32_t audio_queued_frames(void) { return ch < 0 ? 0 : (wr - rd) + AUD_BLOCK; }

void audio_close(void) {
    if (ch < 0) return;
    sa1_log("audio: %u waits for the game (%.1f s in all), %u frames dropped", starves, starve_us / 1e6, dropped);
    run = 0;
    if (sema >= 0) sceKernelSignalSema(sema, 1);
    if (th >= 0) { sceKernelWaitThreadEnd(th, NULL); sceKernelDeleteThread(th); th = -1; }
    if (audio_psp_out2) sceAudioOutput2Release(); else sceAudioChRelease(ch);
    ch = -1;
}
