/* audio_null.c - weak defaults for backend/audio.h: no device.  A backend
 * that plays sound defines the four functions and replaces these at link
 * time (the same mechanism as the seam stubs).
 *
 * Part of sa1-psp (MIT). */
#include "../../backend/audio.h"

#if defined(__GNUC__) || defined(__clang__)
#define AUDIO_WEAK __attribute__((weak))
#else
#define AUDIO_WEAK
#endif

AUDIO_WEAK int audio_open(uint32_t rate, uint32_t channels) { (void)rate; (void)channels; return -1; }
AUDIO_WEAK void audio_push(const int16_t *samples, uint32_t frames) { (void)samples; (void)frames; }
AUDIO_WEAK uint32_t audio_queued_frames(void) { return 0; }
AUDIO_WEAK void audio_close(void) { }
