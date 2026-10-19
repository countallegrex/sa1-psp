/* audio_sdl.c - backend/audio.h on SDL2's audio queue.
 *
 * The HLE pushes one guest frame of samples (735 at 44.1 kHz) per frame;
 * the window paces frames at 60 Hz, so the queue stays near its start
 * level.  When the game runs faster than real time (an unpaced or headless
 * run with sound forced on) the queue is capped at QUEUE_MAX and the excess
 * is dropped: what is heard can skip, what the guest sees cannot, since the
 * audio core's time is counted in rendered samples, not played ones.
 *
 * Part of sa1-psp (MIT). */
#include "../audio.h"
#include "hle.h"

int audio_sdl_enabled = 1;    /* main.c: 0 for --headless and --no-audio */

#ifndef SA1_NO_SDL
#include <SDL.h>

#define QUEUE_START 2048u     /* frames of silence queued at open: the steady-state latency */
#define QUEUE_MAX 4096u       /* frames (93 ms): beyond this a push is dropped, so latency cannot drift */

static SDL_AudioDeviceID dev;
static uint32_t device_frames;

int audio_open(uint32_t rate, uint32_t channels) {
    if (!audio_sdl_enabled) return -1;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        sa1_log("audio: SDL audio init failed: %s", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = (int)rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples = 512;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { sa1_log("audio: no device: %s", SDL_GetError()); return -1; }
    device_frames = have.samples;
    static int16_t silence[2 * QUEUE_START];
    SDL_QueueAudio(dev, silence, sizeof silence);
    SDL_PauseAudioDevice(dev, 0);
    sa1_log("audio: SDL device %d Hz, %d ch, %u-frame buffer, %u frames queued", have.freq, have.channels, device_frames, QUEUE_START);
    return 0;
}

void audio_push(const int16_t *samples, uint32_t frames) {
    if (!dev) return;
    if (SDL_GetQueuedAudioSize(dev) / 4 > QUEUE_MAX) return;
    SDL_QueueAudio(dev, samples, frames * 4);
}

uint32_t audio_queued_frames(void) { return dev ? SDL_GetQueuedAudioSize(dev) / 4 + device_frames : 0; }

void audio_close(void) {
    if (dev) SDL_CloseAudioDevice(dev);
    dev = 0;
}
#endif /* SA1_NO_SDL: hle/sound/audio_null.c's weak defaults stand */
