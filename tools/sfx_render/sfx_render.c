/* sfx_render.c - renders the programs of a multi-unit sound file (S-MLT) to
 * WAV through the audio core (hle/sound/snd_core.c), the same code the game
 * plays them with.  Driven by tools/sfx_render.py.
 *
 *   sfx_render FILE.MLT OUTDIR [BANK [PROGRAM]] [--key K] [--hold SECONDS]
 *
 * Every SMPB unit of the file is copied to its sound-RAM address, then each
 * program is played as the game plays an effect (bank select, program
 * change, note on at key K, default 60, velocity 127), held for --hold
 * seconds (default 1) and released; the render stops when the voices have
 * ended or after 6 s.  Writes OUTDIR/b<bank>_p<program>.wav and prints one
 * line per clip: bank, program, seconds, peak.
 *
 * Part of sa1-psp (MIT). */
#include "snd_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void wav_write(const char *path, const int16_t *s, uint32_t frames) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    uint32_t data = frames * 4, riff = 36 + data;
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                      0x44,0xAC,0,0, 0x10,0xB1,2,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    for (int i = 0; i < 4; i++) { h[4 + i] = (uint8_t)(riff >> (8 * i)); h[40 + i] = (uint8_t)(data >> (8 * i)); }
    fwrite(h, 1, 44, f);
    fwrite(s, 4, frames, f);
    fclose(f);
}

static void midi(snd_shared *sh, int st, int d1, int d2) { snd_cmd c = { SND_CMD_MIDI, 0, (uint8_t)st, (uint8_t)d1, d2, 0 }; snd_cmd_push(sh, &c); }

int main(int argc, char **argv) {
    const char *mlt = NULL, *outdir = NULL;
    int want_bank = -1, want_prog = -1, key = 60;
    double hold = 1.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--key") && i + 1 < argc) key = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atof(argv[++i]);
        else if (!mlt) mlt = argv[i];
        else if (!outdir) outdir = argv[i];
        else if (want_bank < 0) want_bank = atoi(argv[i]);
        else want_prog = atoi(argv[i]);
    }
    if (!mlt || !outdir) { fprintf(stderr, "usage: sfx_render FILE.MLT OUTDIR [BANK [PROGRAM]] [--key K] [--hold S]\n"); return 2; }
    FILE *f = fopen(mlt, "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", mlt); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);
    if (n < 0x20 || memcmp(d, "SM" "LT", 4)) { fprintf(stderr, "%s: not a multi-unit (S-MLT) file\n", mlt); return 1; }
    snd_shared *sh = calloc(1, sizeof *sh);
    sh->sound_ram = calloc(1, SND_RAM_SIZE);
    snd_core *core = calloc(1, snd_core_size());
    uint32_t units = rd32(d + 8);
    for (uint32_t u = 0; u < units && 0x40 + 0x20 * u <= (uint32_t)n; u++) {
        const uint8_t *e = d + 0x20 + 0x20 * u;
        uint32_t bank = rd32(e + 4), aica = rd32(e + 8), off = rd32(e + 16), size = rd32(e + 20);
        if (off == 0xFFFFFFFFu || !size || off + size > (uint32_t)n || aica + size > SND_RAM_SIZE) continue;
        memcpy(sh->sound_ram + aica, d + off, size);
        if (!memcmp(e, "SMPB", 4) && bank < 16) { sh->bank_addr[bank] = aica; sh->bank_size[bank] = size; }
    }
    enum { MAXF = 6 * SND_RATE };
    int16_t *out = malloc((size_t)MAXF * 4);
    for (int bank = 0; bank < 16; bank++) {
        if (!sh->bank_size[bank] || (want_bank >= 0 && bank != want_bank)) continue;
        uint32_t nprog = rd32(sh->sound_ram + sh->bank_addr[bank] + 0x14);
        for (uint32_t prog = 0; prog < nprog && prog < 128; prog++) {
            if (want_prog >= 0 && (int)prog != want_prog) continue;
            snd_core_init(core, sh);
            midi(sh, 0xB0, 32, bank); midi(sh, 0xC0, (int)prog, 0); midi(sh, 0x90, key, 127);
            uint32_t frames = 0, hold_f = (uint32_t)(hold * SND_RATE);
            int released = 0;
            while (frames + SND_FRAME_SAMPLES <= MAXF) {
                if (!released && frames >= hold_f) { midi(sh, 0x80, key, 0); released = 1; }
                snd_core_render(core, out + 2 * frames, SND_FRAME_SAMPLES);
                frames += SND_FRAME_SAMPLES;
                if (sh->voices_active == 0 && (released || frames > SND_FRAME_SAMPLES)) break;
            }
            int peak = 0;
            for (uint32_t i = 0; i < 2 * frames; i++) { int a = out[i] < 0 ? -out[i] : out[i]; if (a > peak) peak = a; }
            char path[600];
            snprintf(path, sizeof path, "%s/b%d_p%u.wav", outdir, bank, prog);
            wav_write(path, out, frames);
            printf("%d\t%u\t%.3f\t%d\n", bank, prog, frames / (double)SND_RATE, peak);
        }
    }
    return 0;
}
