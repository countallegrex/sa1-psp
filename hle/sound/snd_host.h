/* snd_host.h - the main-side half of sound: owns the audio core's shared
 * block and sound RAM, feeds the ADX streams from the files, runs the core
 * once per guest frame and hands the samples to the backend (backend/audio.h)
 * and, when asked, to a WAV file.  The guest-facing entries (sd.c, adxt.c)
 * talk to the core only through here.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>
#include "snd_core.h"
#include "../state.h"

void snd_host_init(void);           /* hle_init */
void snd_host_frame(void);          /* one guest frame (hle/shinobi/sys.c) */
void snd_host_state(hle_state_io *io);
void snd_host_report(void);         /* exit report: counts, CPU cost, latency */

snd_shared *snd_host_shared(void);
void snd_host_cmd(uint8_t op, uint8_t a, uint8_t b, uint8_t c, int32_t x, int32_t y);

/* Streams.  path is data-root relative ("SONICADV/S_SQUARE.ADX"); off and
 * size select an ADX inside a container (an AFS entry), size 0 = to the end
 * of the file.  Returns 0, or -1 when the file is missing or not an ADX. */
int snd_stream_start(int si, const char *path, uint32_t off, uint32_t size);
void snd_stream_stop(int si);
void snd_stream_volume(int si, int32_t level_db10);
/* What the guest sees: the ADXT status (0 stopped, 3 playing, 5 ended) and
 * the time (source samples played and their rate). */
uint32_t snd_stream_state(int si);
void snd_stream_time(int si, uint32_t *count, uint32_t *rate);

/* Options set by the backend before hle_init. */
void snd_host_set_wav(const char *path);    /* record everything rendered */
/* Voice language: -1 = what the game chooses (English on the US disc),
 * 0 = Japanese, 1 = English (docs/m3-audio-report.md section 5). */
extern int snd_voice_lang;
/* The backend's command-line choice (-2 = none): applied over sa1psp.ini. */
extern int snd_voice_lang_cli;
/* What the guest sees of playback (backend/desktop/input_tape.c,
 * --sound-timing): 1 = the players take their streams' real length (M3);
 * 0 = every player reports stopped and no time, which is what the game saw
 * before M3 (no ADX player existed), so tapes recorded then replay
 * unchanged.  The sound itself is rendered the same way in both. */
extern int snd_guest_timing;
/* sa1psp.ini "audio = off": nothing is decoded or mixed; the guest still
 * sees the players take their streams' length (snd_guest_timing). */
extern int snd_audio_enabled;
/* Host time spent in snd_core_render, microseconds, and frames rendered
 * (the CPU-cost line of the M3 report). */
extern uint64_t snd_render_us, snd_render_frames;
