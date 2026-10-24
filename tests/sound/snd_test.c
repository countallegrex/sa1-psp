/* snd_test.c - the audio core on synthetic data (no game data).
 *
 *  1. ADX: a sine and a chirp are encoded here with a small encoder of our
 *     own (the inverse of adx.c's predictor), parsed and decoded by adx.c;
 *     the decode must match the encoder's own reconstruction exactly and the
 *     source within the format's noise.
 *  2. Streams: the same file played through snd_core (ring buffer, block
 *     groups fed in play order, loop) must give, sample for sample, the
 *     decoded stream with the loop applied, at 44.1 kHz and at 22.05 kHz
 *     (resampled: compared against linear interpolation of the decode).
 *  3. Tones: a synthetic program bank (SMPB with one program, one layer,
 *     one PCM16 split and one ADPCM split) in sound RAM; MIDI note-ons
 *     must sound at the pitch the key, base key and bend give, and stop on
 *     note-off.
 *  4. Determinism: two identical runs give identical output.
 *
 * Part of sa1-psp (MIT). */
#include "snd_core.h"
#include "adx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- a minimal ADX encoder ------------------------------------------------ */
typedef struct { uint8_t *data; uint32_t size; int16_t *recon; } adx_file;

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* Encodes ch channels of n samples (interleaved src) at rate; loop if ls < le. */
static adx_file encode(const int16_t *src, uint32_t n, uint32_t ch, uint32_t rate, uint32_t ls, uint32_t le) {
    adx_file f;
    uint32_t blocks = (n + 31) / 32, hdr = 0x34;           /* copyright offset 0x30 -> data at 0x34 */
    f.size = hdr + blocks * 18 * ch;
    f.data = calloc(1, f.size);
    f.recon = calloc((size_t)blocks * 32 * ch, sizeof(int16_t));
    uint8_t *h = f.data;
    put16(h, 0x8000); put16(h + 2, hdr - 4);
    h[4] = 3; h[5] = 18; h[6] = 4; h[7] = (uint8_t)ch;
    put32(h + 8, rate); put32(h + 12, n); put16(h + 16, 500);
    h[0x12] = 3; h[0x13] = 0;
    if (ls < le) { put16(h + 0x14, 0); put16(h + 0x16, 1); put32(h + 0x18, 1); put32(h + 0x1C, ls); put32(h + 0x20, hdr + (ls / 32) * 18 * ch); put32(h + 0x24, le); put32(h + 0x28, hdr + ((le + 31) / 32) * 18 * ch); }
    memcpy(h + 0x2E, "(c)" "CRI", 6);
    adx_info info;
    adx_parse(h, hdr, &info);
    for (uint32_t c = 0; c < ch; c++) {
        int32_t h1 = 0, h2 = 0;
        for (uint32_t b = 0; b < blocks; b++) {
            /* scale: largest prediction residual over 7, from an open-loop pass */
            int32_t p1 = h1, p2 = h2, maxr = 0;
            for (int i = 0; i < 32; i++) {
                uint32_t k = b * 32 + i;
                int32_t x = k < n ? src[k * ch + c] : 0;
                int32_t pred = (info.coef1 * p1 + info.coef2 * p2) >> 12;
                int32_t r = abs(x - pred);
                if (r > maxr) maxr = r;
                p2 = p1; p1 = x;
            }
            int32_t scale = (maxr + 6) / 7;
            if (scale < 1) scale = 1;
            if (scale > 0x7FFF) scale = 0x7FFF;
            uint8_t *blk = f.data + hdr + (b * ch + c) * 18;
            put16(blk, (uint32_t)scale);
            for (int i = 0; i < 32; i++) {
                uint32_t k = b * 32 + i;
                int32_t x = k < n ? src[k * ch + c] : 0;
                int32_t pred = (info.coef1 * h1 + info.coef2 * h2) >> 12;
                int32_t q = (int32_t)lrint((double)(x - pred) / scale);
                if (q > 7) q = 7;
                if (q < -8) q = -8;
                int32_t y = q * scale + pred;
                if (y > 32767) y = 32767;
                if (y < -32768) y = -32768;
                blk[2 + i / 2] |= (uint8_t)((q & 15) << ((i & 1) ? 0 : 4));
                f.recon[k * ch + c] = (int16_t)y;
                h2 = h1; h1 = y;
            }
        }
    }
    return f;
}

static int16_t *decode_all(const adx_file *f, adx_info *info) {
    adx_parse(f->data, f->size, info);
    uint32_t blocks = (info->samples + 31) / 32, ch = info->channels;
    int16_t *out = calloc((size_t)blocks * 32 * ch, sizeof(int16_t));
    adx_hist hist[2] = { { 0, 0 }, { 0, 0 } };
    int16_t tmp[32];
    for (uint32_t b = 0; b < blocks; b++)
        for (uint32_t c = 0; c < ch; c++) {
            adx_decode_block(f->data + info->data_off + (b * ch + c) * 18, info->coef1, info->coef2, &hist[c], tmp);
            for (int i = 0; i < 32; i++) out[(b * 32 + i) * ch + c] = tmp[i];
        }
    return out;
}

static void test_decoder(void) {
    enum { N = 44100 };
    int16_t *src = malloc(N * 2 * sizeof(int16_t));
    for (int i = 0; i < N; i++) {
        src[2 * i] = (int16_t)(12000 * sin(2 * M_PI * 440 * i / 44100.0));
        double f = 100 + 4000.0 * i / N;                     /* chirp */
        src[2 * i + 1] = (int16_t)(9000 * sin(2 * M_PI * f * i / 44100.0));
    }
    adx_file f = encode(src, N, 2, 44100, 0, 0);
    adx_info info;
    CHECK(adx_parse(f.data, f.size, &info) == 0, "parse");
    CHECK(info.channels == 2 && info.rate == 44100 && info.samples == N && !info.loop, "header fields");
    CHECK(info.data_off == 0x34, "data offset %u", info.data_off);
    int16_t *dec = decode_all(&f, &info);
    uint32_t mism = 0; double err = 0, sig = 0;
    for (int i = 0; i < 2 * N; i++) {
        if (dec[i] != f.recon[i]) mism++;
        double d = dec[i] - src[i]; err += d * d; sig += (double)src[i] * src[i];
    }
    double snr = 10 * log10(sig / (err + 1));
    CHECK(mism == 0, "decoder differs from the encoder's reconstruction in %u samples", mism);
    CHECK(snr > 20, "SNR %.1f dB", snr);
    printf("adx: decode exact, SNR %.1f dB against the source\n", snr);
    uint8_t bad[0x34]; memcpy(bad, f.data, sizeof bad); bad[0x13] = 8;
    CHECK(adx_parse(bad, sizeof bad, &info) != 0, "encrypted header refused");
    free(src); free(dec); free(f.data); free(f.recon);
}

/* ---- streams through the core ---------------------------------------------- */
static snd_shared *sh_new(void) {
    snd_shared *sh = calloc(1, sizeof *sh);
    sh->sound_ram = calloc(1, SND_RAM_SIZE);
    return sh;
}

/* Feeds block groups in play order (snd_core.h, the loop rule). */
typedef struct { const adx_file *f; adx_info info; uint32_t q; } feeder;
static void feed(snd_shared *sh, int si, feeder *fd) {
    uint32_t gb = adx_group_bytes(&fd->info), G = adx_play_groups(&fd->info), L = adx_group_of(fd->info.loop_start);
    while (snd_ring_free(&sh->stream_data[si]) >= gb && (fd->info.loop || fd->q < G)) {
        uint32_t g = fd->q < G ? fd->q : L + (fd->q - G) % (G - L);
        snd_ring_write(&sh->stream_data[si], fd->f->data + fd->info.data_off + g * gb, gb);
        fd->q++;
    }
}

static void test_stream(uint32_t rate, int loop) {
    enum { N = 30000 };
    int16_t *src = malloc(N * sizeof(int16_t));
    for (int i = 0; i < N; i++) src[i] = (int16_t)(10000 * sin(2 * M_PI * 300 * i / (double)rate) + 3000 * sin(2 * M_PI * 1234 * i / (double)rate));
    uint32_t ls = loop ? 7013 : 0, le = loop ? 23999 : 0;   /* neither on a block boundary */
    adx_file f = encode(src, N, 1, rate, ls, le);
    adx_info info;
    int16_t *dec = decode_all(&f, &info);
    CHECK(info.loop == (uint32_t)loop && (!loop || (info.loop_start == ls && info.loop_end == le)), "loop fields");
    /* the expected source sequence: 0..le-1, then ls..le-1 again and again */
    enum { OUT = 3 * N };
    int16_t *want = malloc(OUT * sizeof(int16_t));
    uint32_t s = 0, nwant = 0;
    for (; nwant < OUT; nwant++) {
        uint32_t end = loop ? le : N;
        if (s >= end) { if (!loop) break; s = ls; }
        want[nwant] = dec[s++];
    }
    snd_shared *sh = sh_new();
    snd_core *core = calloc(1, snd_core_size());
    snd_core_init(core, sh);
    sh->stream_info[1] = info;
    feeder fd = { &f, info, 0 };
    feed(sh, 1, &fd);
    snd_cmd c = { SND_CMD_STREAM_START, 1, 0, 0, 0, 7 };
    snd_cmd_push(sh, &c);
    uint32_t outn = (uint32_t)((uint64_t)OUT * SND_RATE / rate) + SND_FRAME_SAMPLES;
    int16_t *out = malloc((size_t)outn * 2 * sizeof(int16_t));
    uint32_t done = 0;
    while (done + SND_FRAME_SAMPLES <= outn) {
        snd_core_render(core, out + 2 * done, SND_FRAME_SAMPLES);
        done += SND_FRAME_SAMPLES;
        feed(sh, 1, &fd);
    }
    /* output sample j interpolates source samples floor(t) - 1 and floor(t)
     * with t = 1 + j * rate / 44100 (the core fetches the first sample at
     * once; see stream_render) */
    uint32_t step = (uint32_t)(((uint64_t)rate << 16) / SND_RATE), mism = 0, compared = 0;
    uint64_t pos = 0x10000;
    int32_t prev = 0, cur = 0; uint32_t k = 0;
    for (uint32_t j = 0; j < done; j++) {
        while (pos >= 0x10000) { pos -= 0x10000; prev = cur; cur = k < nwant ? want[k] : 0; k++; }
        if (k > nwant) break;
        int32_t t = (int32_t)(pos >> 1);
        int32_t e = prev + (int32_t)(((int64_t)(cur - prev) * t) >> 15);
        if (out[2 * j] != e || out[2 * j + 1] != e) { if (mism < 3) printf("  sample %u: got %d/%d want %d\n", j, out[2 * j], out[2 * j + 1], e); mism++; }
        compared++;
        pos += step;
    }
    CHECK(mism == 0, "stream at %u Hz%s: %u of %u samples differ", rate, loop ? " (looped)" : "", mism, compared);
    CHECK(compared > (loop ? (uint32_t)(2 * N * SND_RATE / rate) : (uint32_t)(N * SND_RATE / rate) - 2), "compared only %u samples", compared);
    uint32_t want_state = loop ? 3 : 5;
    CHECK(sh->status[1].state == want_state, "status %u, want %u", sh->status[1].state, want_state);
    CHECK(sh->status[1].generation == 7, "generation");
    printf("stream %u Hz%s: %u samples exact%s\n", rate, loop ? " looped" : "", compared, loop ? "" : ", ends in state 5");
    free(src); free(dec); free(want); free(out); free(f.data); free(f.recon); free(core); free(sh->sound_ram); free(sh);
}

/* ---- tones ------------------------------------------------------------------ */
static void w8(uint8_t *p, uint32_t a, uint32_t v) { p[a] = (uint8_t)v; }
static void w16(uint8_t *p, uint32_t a, uint32_t v) { w8(p, a, v); w8(p, a + 1, v >> 8); }
static void w32(uint8_t *p, uint32_t a, uint32_t v) { w16(p, a, v); w16(p, a + 2, v >> 16); }

/* One bank at sound-RAM address B: program 0 = PCM16 sine (base key 72,
 * looped), program 1 = the same tone as ADPCM (one-shot). */
enum { B = 0x12C000, TONE_LEN = 4410 };
static void build_bank(uint8_t *ram) {
    uint32_t prog_tab = 0x30, p0 = 0x40, p1 = 0x58, l0 = 0x70, l1 = 0x80, s0 = 0x90, s1 = 0xC0, tone0 = 0x100, tone1 = tone0 + 2 * TONE_LEN;
    memcpy(ram + B, "SMPB", 4);
    w32(ram, B + 4, 1); w32(ram, B + 0x10, prog_tab); w32(ram, B + 0x14, 2);
    w32(ram, B + prog_tab, p0); w32(ram, B + prog_tab + 4, p1);
    w32(ram, B + p0, l0); w32(ram, B + p1, l1);
    w32(ram, B + l0, 1); w32(ram, B + l0 + 4, s0); w8(ram, B + l0 + 12, 2); w8(ram, B + l0 + 13, 2);
    w32(ram, B + l1, 1); w32(ram, B + l1 + 4, s1); w8(ram, B + l1 + 12, 2); w8(ram, B + l1 + 13, 2);
    /* a 100-sample period: 441 Hz at the native rate */
    for (int i = 0; i < TONE_LEN; i++) w16(ram, B + tone0 + 2 * i, (uint32_t)(int16_t)(16000 * sin(2 * M_PI * i / 100.0)));
    /* the same as ADPCM, encoded with the decoder's own rule */
    int32_t s = 0, q = 127;
    static const int32_t sc[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    for (int i = 0; i < TONE_LEN; i++) {
        int32_t x = (int32_t)(16000 * sin(2 * M_PI * i / 100.0)), best = 0, bestd = 1 << 30;
        for (int nib = 0; nib < 16; nib++) {
            int32_t m = nib & 7, d = ((1 + 2 * m) * q) >> 3, y = (nib & 8) ? s - d : s + d;
            if (abs(y - x) < bestd) { bestd = abs(y - x); best = nib; }
        }
        int32_t m = best & 7, d = ((1 + 2 * m) * q) >> 3;
        s = (best & 8) ? s - d : s + d; if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        q = (q * sc[m]) >> 8; if (q < 127) q = 127; if (q > 24576) q = 24576;
        ram[B + tone1 + i / 2] |= (uint8_t)(best << ((i & 1) ? 4 : 0));
    }
    uint32_t sp[2] = { s0, s1 }, tone[2] = { tone0, tone1 }, w0[2] = { 0x0200, 0x0100 };   /* PCM16 + loop; ADPCM */
    for (int k = 0; k < 2; k++) {
        uint32_t a = B + sp[k];
        w16(ram, a, w0[k] | (tone[k] >> 16)); w16(ram, a + 2, tone[k] & 0xFFFF);
        w16(ram, a + 4, 0); w16(ram, a + 6, TONE_LEN);
        w16(ram, a + 8, 0x001F);            /* AR 31, D1R 0, D2R 0 */
        w16(ram, a + 10, 0x001F);           /* RR 31 */
        w16(ram, a + 18, 0x0F00);           /* DISDL 15, centre */
        w16(ram, a + 20, 0x0000);           /* TL 0 */
        w8(ram, a + 36, 0); w8(ram, a + 37, 127); w8(ram, a + 38, 72); w8(ram, a + 39, 0);
    }
}

static void midi(snd_shared *sh, int port, int st, int d1, int d2) { snd_cmd c = { SND_CMD_MIDI, (uint8_t)port, (uint8_t)st, (uint8_t)d1, d2, 0 }; snd_cmd_push(sh, &c); }

/* Rising zero crossings per second of the left channel. */
static double freq_of(const int16_t *out, uint32_t n) {
    int z = 0, first = -1, last = -1;
    for (uint32_t i = 1; i < n; i++) if (out[2 * (i - 1)] < 0 && out[2 * i] >= 0) { if (first < 0) first = (int)i; last = (int)i; z++; }
    return z > 1 ? (z - 1) * (double)SND_RATE / (last - first) : 0;
}

static double rms_of(const int16_t *out, uint32_t n) { double a = 0; for (uint32_t i = 0; i < n; i++) a += (double)out[2 * i] * out[2 * i]; return sqrt(a / n); }

static void test_tones(void) {
    snd_shared *sh = sh_new();
    build_bank(sh->sound_ram);
    sh->bank_addr[4] = B; sh->bank_size[4] = 0x10000;
    snd_core *core = calloc(1, snd_core_size());
    snd_core_init(core, sh);
    static int16_t out[2 * SND_RATE];
    /* program 0, key 72 = base: 441 Hz; key 60: 220.5 Hz */
    midi(sh, 0, 0xB3, 32, 4); midi(sh, 0, 0xC3, 0, 0); midi(sh, 0, 0xB3, 7, 127); midi(sh, 0, 0x93, 72, 127);
    snd_core_render(core, out, SND_RATE / 2);
    double f1 = freq_of(out, SND_RATE / 2), r1 = rms_of(out, SND_RATE / 2);
    CHECK(fabs(f1 - 441) < 3, "key 72: %.1f Hz, want 441", f1);
    CHECK(r1 > 5000, "key 72: rms %.0f", r1);
    midi(sh, 0, 0x83, 72, 0);
    snd_core_render(core, out, SND_RATE / 10);
    double r_off = rms_of(out + 2 * (SND_RATE / 20), SND_RATE / 20);
    CHECK(r_off < 10, "note-off: still %.0f rms 50 ms later", r_off);
    midi(sh, 0, 0x93, 60, 127);
    snd_core_render(core, out, SND_RATE / 2);
    double f2 = freq_of(out, SND_RATE / 2);
    CHECK(fabs(f2 - 220.5) < 2, "key 60: %.1f Hz, want 220.5", f2);
    /* pitch bend up two semitones (full range of 2): 247.5 Hz */
    midi(sh, 0, 0xE3, 0x7F, 0x7F);
    snd_core_render(core, out, SND_RATE / 2);
    double f3 = freq_of(out, SND_RATE / 2);
    CHECK(fabs(f3 - 220.5 * pow(2, 2 * 8191.0 / 8192 / 12)) < 2.5, "bent: %.1f Hz", f3);
    midi(sh, 0, 0x83, 60, 0); midi(sh, 0, 0xE3, 0, 0x40);
    snd_core_render(core, out, SND_RATE / 10);
    /* program 1 (ADPCM, one-shot of 0.1 s at 441 Hz): sounds, then ends by itself */
    midi(sh, 1, 0xB0, 32, 4); midi(sh, 1, 0xC0, 1, 0); midi(sh, 1, 0xB0, 7, 127); midi(sh, 1, 0x90, 72, 127);
    snd_core_render(core, out, SND_RATE / 20);
    double f4 = freq_of(out, SND_RATE / 20), r4 = rms_of(out, SND_RATE / 20);
    CHECK(fabs(f4 - 441) < 10 && r4 > 3000, "ADPCM tone: %.1f Hz rms %.0f", f4, r4);
    snd_core_render(core, out, SND_RATE / 10);
    CHECK(sh->voices_active == 0, "one-shot still active");
    /* the driver's level law (snd_core.c tone_att), no bank curve: CC 7 100
     * gives level ((127+1)(100+1)(256-0)-1) >> 14 = 201, TL 54, 20.25 dB under
     * CC 7 127 (level 255, TL 0) */
    midi(sh, 1, 0xB0, 7, 100); midi(sh, 1, 0x90, 72, 127);
    snd_core_render(core, out, SND_RATE / 20);
    double r5 = rms_of(out, SND_RATE / 20), db5 = 20 * log10(r5 / r4);
    CHECK(fabs(db5 + 20.25) < 0.5, "ADPCM tone at CC 7 100: %.2f dB, want -20.25", db5);
    snd_core_render(core, out, SND_RATE / 10);
    printf("tones: key 72 %.1f Hz, key 60 %.1f Hz, bent %.1f Hz, ADPCM %.1f Hz, releases end\n", f1, f2, f3, f4);
    free(core); free(sh->sound_ram); free(sh);
}

/* Two runs of the same commands: the same bytes. */
static uint32_t run_hash(void) {
    snd_shared *sh = sh_new();
    build_bank(sh->sound_ram);
    sh->bank_addr[4] = B; sh->bank_size[4] = 0x10000;
    snd_core *core = calloc(1, snd_core_size());
    snd_core_init(core, sh);
    static int16_t out[2 * SND_FRAME_SAMPLES];
    uint32_t h = 2166136261u;
    for (int fr = 0; fr < 120; fr++) {
        if (fr % 10 == 0) { midi(sh, 0, 0xB0 | (fr / 10 % 16), 32, 4); midi(sh, 0, 0xC0 | (fr / 10 % 16), fr / 10 % 2, 0); midi(sh, 0, 0x90 | (fr / 10 % 16), 50 + fr / 10, 100); }
        snd_core_render(core, out, SND_FRAME_SAMPLES);
        for (int i = 0; i < 2 * SND_FRAME_SAMPLES; i++) h = (h ^ (uint16_t)out[i]) * 16777619u;
    }
    free(core); free(sh->sound_ram); free(sh);
    return h;
}

int main(void) {
    test_decoder();
    test_stream(44100, 0);
    test_stream(44100, 1);
    test_stream(22050, 1);
    test_tones();
    uint32_t a = run_hash(), b = run_hash();
    CHECK(a == b, "two identical runs differ (%08x %08x)", a, b);
    printf("determinism: %08x twice\n", a);
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all sound checks passed\n");
    return 0;
}
