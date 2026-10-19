/* audio.h - the sound output a backend provides.  One implementation per
 * target: backend/desktop/audio_sdl.c (SDL2 audio queue) and, later, the
 * PSP's sceAudio.  hle/sound/audio_null.c holds weak defaults (no device),
 * so a backend without sound still links and the game still runs the audio
 * core: what the guest sees of sound (a voice line still playing, the BGM
 * time) never depends on whether a device exists.
 *
 * The HLE calls these from the game's thread, once per guest frame, with
 * 735 frames of 44.1 kHz interleaved stereo signed 16-bit samples.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>

/* Opens the device; 0 on success.  rate is 44100, channels 2. */
int audio_open(uint32_t rate, uint32_t channels);
/* Queues frames (rate x channels interleaved); never blocks.  A backend may
 * drop samples when its queue is already long (an unpaced run). */
void audio_push(const int16_t *samples, uint32_t frames);
/* Frames queued and not yet heard: the output latency. */
uint32_t audio_queued_frames(void);
void audio_close(void);
