/* snd_core.c - the audio core (see snd_core.h): tone voices that read the
 * program banks in sound RAM the way the AICA reads them, the MIDI channel
 * layer the game drives them with, two ADX stream players, and the mixer.
 *
 * Formats (docs/formats/sound.md): a program bank (SMPB) holds programs of
 * up to four layers; a layer holds splits by key range; a split is the
 * AICA channel registers of one tone (sample address relative to the bank,
 * format, loop points, envelope rates, send levels) plus its key range,
 * base key and fine tune.  A MIDI note-on on a channel starts one voice per
 * layer of the channel's program in the channel's bank (bank select, CC 32).
 *
 * Levels are kept as attenuation in the AICA's envelope unit (1/1024 of
 * 96 dB, 0.09375 dB) and turned into a Q15 gain by one table lookup per
 * sample, which is how the hardware composes envelope, total level, send
 * level and pan.
 *
 * Part of sa1-psp (MIT). */
#include "snd_core.h"
#include <string.h>
#include <math.h>

#define ATT_MAX 1023
#define ATT_DB(db) ((int32_t)((db) / 0.09375 + 0.5))

/* Console calibration (docs/m3-audio-report.md section 6: our stems
 * fitted against Flycast captures of the same game frames): music 0.6 dB
 * lower.  Voices matched; tones follow the driver's level law (tone_att)
 * and need no trim. */
#define TRIM_MUSIC_ATT ATT_DB(0.6)

enum { EG_OFF = 0, EG_ATTACK, EG_DECAY1, EG_DECAY2, EG_RELEASE };
enum { FMT_PCM16 = 0, FMT_PCM8 = 1, FMT_ADPCM = 2 };

typedef struct voice {
    uint8_t on, port, ch, key;
    uint8_t fmt, loop, eg_stage, layer;
    uint32_t age;                   /* note-on order, for stealing */
    uint32_t addr;                  /* sample 0 in sound RAM */
    uint32_t lsa, lea;              /* loop start / end, samples */
    uint32_t pos;                   /* integer sample index of cur */
    uint32_t frac;                  /* Q16 between cur and nxt */
    uint32_t step;                  /* Q16 source samples per output sample (without bend) */
    int32_t cur, nxt;               /* samples at pos and pos + 1 */
    int32_t ad_s, ad_q;             /* ADPCM decoder: last sample, step size (state after nxt) */
    int32_t ad_ls, ad_lq;           /* ADPCM state saved at the loop start */
    uint32_t ad_next;               /* index of the next sample the ADPCM decoder produces */
    uint8_t ad_saved;
    int32_t eg;                     /* attenuation, Q16 units of ATT (0 loudest) */
    int32_t eg_ar, eg_d1, eg_d2, eg_rr, eg_dl;   /* per-sample steps (Q16 att units), DL in att units */
    int32_t att_static;             /* direct send level (both sides) */
    int32_t base_cents;             /* key - base key + fine tune */
    uint8_t bend_range;             /* semitones */
    uint8_t span;                   /* the split's own pan on the CC 10 scale (DIPAN x 4) */
    uint8_t tl;                     /* the split's TL */
    uint8_t vlev;                   /* velocity curve value + 1 (1..128) */
    uint32_t delay;                 /* output samples before the voice starts (layer delay) */
    uint16_t pitch_reg;             /* the split's OCT/FNS word */
    uint8_t bank, prog;             /* what started it (the solo debug filter) */
} voice;

typedef struct chan {
    uint8_t bank, prog, vol, pan, expr, mod;   /* pan bit 7: no CC 10 yet, the split's own pan */
    int16_t bend;                   /* -8192 .. 8191 */
} chan;

typedef struct stream {
    uint8_t on, finished, reload, saved_ok;
    uint32_t generation;
    adx_info info;
    uint32_t tail;                  /* ring read position */
    uint32_t s;                     /* next source sample */
    uint32_t blk_start;
    uint8_t blk_valid;
    adx_hist hist[ADX_MAX_CH], saved[ADX_MAX_CH];
    int16_t blk[ADX_MAX_CH][ADX_BLOCK_SAMPLES];
    int32_t prev[ADX_MAX_CH], cur[ADX_MAX_CH];
    uint32_t frac, step;
    int32_t att;                    /* output level, att units */
    uint32_t played, decoded;
} stream;

struct snd_core {
    snd_shared *sh;
    voice v[SND_VOICES];
    chan ch[SND_PORTS][16];
    stream st[SND_STREAMS];
    int32_t port_att[4];
    uint32_t age;
};

/* Tables, built once (snd_core_init; identical on every run). */
static int32_t gain_q15[ATT_MAX + 1];   /* Q15, 32768 = unity */
static uint16_t pan_att[128];       /* attenuation of the far side for pan 0..127 (DIPAN steps) */
static uint32_t cents_q16[1200];    /* 2^(c/1200), Q16 */
static int32_t eg_rate_step[64];    /* Q16 att units per sample for a full-range decay at effective rate r */
static int tables_ready;

static void build_tables(void) {
    if (tables_ready) return;
    for (int i = 0; i <= ATT_MAX; i++) gain_q15[i] = (int32_t)(32768.0 * pow(10.0, -(i * 0.09375) / 20.0) + 0.5);
    gain_q15[ATT_MAX] = 0;
    /* The driver (MASARU.DRV, key-on and CC 10) sets DIPAN from pan >> 2:
     * below 16 the right side drops by 15 - q steps, from 16 the left by
     * q - 16; a step is 3 dB and step 15 mutes (AICA send levels). */
    for (int p = 0; p < 128; p++) {
        int q = p >> 2, d = q < 16 ? 15 - q : q - 16;
        pan_att[p] = (uint16_t)(d >= 15 ? ATT_MAX : d * 32);
    }
    for (int c = 0; c < 1200; c++) cents_q16[c] = (uint32_t)(65536.0 * pow(2.0, c / 1200.0) + 0.5);
    /* Envelope rates: a full 96 dB decay takes about 118 s at effective
     * rate 2 and halves every four rate steps (3.6 ms at 62).  Effective
     * rate = 2 x the 5-bit register value; 0 stops the envelope. */
    eg_rate_step[0] = 0; eg_rate_step[1] = 0;
    for (int r = 2; r < 64; r++) {
        double t = 118.2 * pow(2.0, -(r - 2) / 4.0);
        eg_rate_step[r] = (int32_t)(ATT_MAX * 65536.0 / (t * SND_RATE) + 0.5);
        if (eg_rate_step[r] < 1) eg_rate_step[r] = 1;
    }
    tables_ready = 1;
}

static inline int32_t clamp_att(int32_t a) { return a < 0 ? 0 : a > ATT_MAX ? ATT_MAX : a; }

/* ---- sound RAM reads ------------------------------------------------------ */
static inline uint32_t ram8(const snd_core *k, uint32_t a) { return a < SND_RAM_SIZE ? k->sh->sound_ram[a] : 0; }
static inline uint32_t ram16(const snd_core *k, uint32_t a) { return ram8(k, a) | ram8(k, a + 1) << 8; }
static inline uint32_t ram32(const snd_core *k, uint32_t a) { return ram16(k, a) | ram16(k, a + 2) << 16; }

/* ---- the AICA's 4-bit ADPCM (the public Yamaha scheme) ------------------- */
static const int32_t adpcm_scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };

static inline void adpcm_step(int32_t nib, int32_t *s, int32_t *q) {
    int32_t m = nib & 7;
    int32_t d = ((1 + 2 * m) * *q) >> 3;
    int32_t x = (nib & 8) ? *s - d : *s + d;
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    *s = x;
    int32_t nq = (*q * adpcm_scale[m]) >> 8;
    if (nq < 127) nq = 127;
    if (nq > 24576) nq = 24576;
    *q = nq;
}

/* Sample i of a voice's tone (i advances by one per call for ADPCM). */
static int32_t tone_sample(snd_core *k, voice *v, uint32_t i) {
    switch (v->fmt) {
    case FMT_PCM16: return (int16_t)ram16(k, v->addr + 2 * i);
    case FMT_PCM8: return (int8_t)ram8(k, v->addr + i) << 8;
    default: {
        /* sequential decode; a loop back restores the state saved at LSA */
        if (i != v->ad_next) {
            if (v->ad_saved && i == v->lsa) { v->ad_s = v->ad_ls; v->ad_q = v->ad_lq; v->ad_next = i; }
            else { v->ad_s = 0; v->ad_q = 127; v->ad_next = 0; while (v->ad_next < i) { uint32_t b = ram8(k, v->addr + v->ad_next / 2); adpcm_step((v->ad_next & 1) ? b >> 4 : b & 15, &v->ad_s, &v->ad_q); v->ad_next++; } }
        }
        if (v->loop && i == v->lsa && !v->ad_saved) { v->ad_ls = v->ad_s; v->ad_lq = v->ad_q; v->ad_saved = 1; }
        uint32_t b = ram8(k, v->addr + i / 2);
        adpcm_step((i & 1) ? b >> 4 : b & 15, &v->ad_s, &v->ad_q);
        v->ad_next = i + 1;
        return v->ad_s;
    }
    }
}

/* Index after i, or -1 at the end of a one-shot tone. */
static inline int64_t tone_next(const voice *v, uint32_t i) {
    uint32_t n = i + 1;
    if (n >= v->lea) return v->loop ? (int64_t)v->lsa : -1;
    return n;
}

/* ---- program lookup ------------------------------------------------------- */
typedef struct split_ref { uint32_t sp, bank_base; uint8_t bend; uint8_t delay; } split_ref;
#define MAX_SPLITS 8

/* The bank's level curve (header +0x18, 128 bytes, 127 -> 127), 0 if none. */
static uint32_t bank_curve(const snd_core *k, uint32_t bank) {
    if (bank >= 16 || !k->sh->bank_size[bank]) return 0;
    uint32_t B = k->sh->bank_addr[bank], t = ram32(k, B + 0x18);
    return t && t < 0x10000 ? B + t : 0;
}

/* Curve n of the bank (the count is at header +0x1C; out of range -> 0),
 * and a value read through a curve (none: v itself). */
static uint32_t bank_curve_n(const snd_core *k, uint32_t bank, uint32_t n) {
    uint32_t c = bank_curve(k, bank);
    if (!c) return 0;
    return n < ram32(k, k->sh->bank_addr[bank] + 0x1C) ? c + 128 * n : c;
}
static inline uint32_t curve_at(const snd_core *k, uint32_t curve, uint32_t v) {
    return curve ? ram8(k, curve + (v & 127)) : v & 127;
}

/* The TL the driver (MASARU.DRV key-on and CC 7) writes, as attenuation:
 * the volume and expression curve values combine to c = (cv+1)(ce+1)/128 - 1,
 * then level = ((curve_vel[vel]+1)(c+1)(256-TL) - 1) >> 14 and TL = 255 -
 * level.  The volume table is the driver's own, which is bank curve 0 but
 * for entries 1-4 (inaudible); the velocity curve is the split's (byte 42).
 * Near TL 0 this is TL + 2 x (127 - curve[v]), as Flycast's registers
 * showed against the game's CC 7 over the Chaos 0 fades and the Emerald
 * Coast ring.  Volume and expression both at curve 0 make the driver's c
 * wrap to 255 (full level); we keep that silent. */
static inline int32_t tone_att(const snd_core *k, const voice *v, const chan *ch) {
    uint32_t curve = bank_curve(k, v->bank);
    int32_t c = (int32_t)(((curve_at(k, curve, ch->vol) + 1) * (curve_at(k, curve, ch->expr) + 1)) >> 7) - 1;
    if (c < 0) c = 0;
    uint32_t level = ((uint32_t)v->vlev * (uint32_t)(c + 1) * (256u - v->tl) - 1) >> 14;
    return (int32_t)(255 - (level & 255)) * 4;
}

/* The splits a note-on starts: in every layer, each split whose key range
 * holds the key (a layer may stack two tones on one range).  Returns the
 * count, at most MAX_SPLITS. */
static int find_splits(snd_core *k, uint32_t bank, uint32_t prog, uint32_t key, split_ref *out) {
    if (bank >= 16 || !k->sh->bank_size[bank]) return 0;
    uint32_t B = k->sh->bank_addr[bank], size = k->sh->bank_size[bank];
    if (ram32(k, B) != 0x42504D53u) return 0;               /* "SMPB" */
    uint32_t ptab = ram32(k, B + 0x10), nprog = ram32(k, B + 0x14);
    if (prog >= nprog || ptab >= size) return 0;
    uint32_t po = ram32(k, B + ptab + 4 * prog);
    if (!po || po >= size) return 0;
    int n = 0;
    for (int l = 0; l < 4; l++) {
        uint32_t lo = ram32(k, B + po + 4 * l);
        if (!lo || lo >= size) continue;
        uint32_t ns = ram32(k, B + lo), so = ram32(k, B + lo + 4);
        for (uint32_t s = 0; s < ns && s < 128; s++) {
            uint32_t sp = B + so + 48 * s;
            if (so + 48 * s + 48 > size) break;
            uint32_t klo = ram8(k, sp + 36), khi = ram8(k, sp + 37);
            if (key < klo || key > khi) continue;
            if (n == MAX_SPLITS) return n;
            out[n].sp = sp; out[n].bank_base = B; out[n].bend = (uint8_t)ram8(k, B + lo + 12);
            out[n].delay = (uint8_t)ram8(k, B + lo + 8);    /* layer start delay, taken as milliseconds */
            n++;
        }
    }
    return n;
}

/* ---- voices ---------------------------------------------------------------- */
static voice *alloc_voice(snd_core *k) {
    voice *best = NULL;
    for (int i = 0; i < SND_VOICES; i++) if (!k->v[i].on) return &k->v[i];
    for (int i = 0; i < SND_VOICES; i++)             /* oldest released */
        if (k->v[i].eg_stage == EG_RELEASE && (!best || k->v[i].age < best->age)) best = &k->v[i];
    if (best) return best;
    for (int i = 0; i < SND_VOICES; i++) if (!best || k->v[i].age < best->age) best = &k->v[i];
    return best;
}

static uint32_t pitch_step(int32_t cents, uint32_t pitch_reg) {
    int32_t oct = 0;
    while (cents < 0) { cents += 1200; oct--; }
    while (cents >= 1200) { cents -= 1200; oct++; }
    uint64_t s = cents_q16[cents];
    /* the split's own OCT/FNS (usually 0): 2^OCT x (1 + FNS/1024) */
    int32_t ro = (int32_t)((pitch_reg >> 11) & 15); if (ro & 8) ro -= 16;
    uint32_t fns = pitch_reg & 0x3FF;
    s = s * (1024 + fns) >> 10;
    oct += ro;
    if (oct >= 0) s <<= oct; else s >>= -oct;
    if (s > 0x7FFFFFFFu) s = 0x7FFFFFFFu;
    return (uint32_t)s;
}

static void note_on(snd_core *k, int port, int c, int key, int vel) {
    chan *ch = &k->ch[port][c];
    split_ref sp[MAX_SPLITS];
    int n = find_splits(k, ch->bank, ch->prog, (uint32_t)key, sp);
    /* a retrigger of the same key on the channel releases the old note */
    for (int i = 0; i < SND_VOICES; i++) {
        voice *v = &k->v[i];
        if (v->on && v->port == port && v->ch == c && v->key == key && v->eg_stage != EG_RELEASE) v->eg_stage = EG_RELEASE;
    }
    for (int l = 0; l < n; l++) {
        uint32_t p = sp[l].sp;
        uint32_t w0 = ram16(k, p);
        voice *v = alloc_voice(k);
        memset(v, 0, sizeof *v);
        v->port = (uint8_t)port; v->ch = (uint8_t)c; v->key = (uint8_t)key; v->layer = (uint8_t)l;
        v->bank = ch->bank; v->prog = ch->prog;
        v->fmt = (uint8_t)((w0 >> 7) & 3); if (v->fmt == 3) v->fmt = FMT_ADPCM;
        v->loop = (uint8_t)((w0 >> 9) & 1);
        v->addr = sp[l].bank_base + (((w0 & 0x7F) << 16) | ram16(k, p + 2));
        v->lsa = ram16(k, p + 4); v->lea = ram16(k, p + 6);
        if (v->lea == 0) continue;
        if (v->lsa >= v->lea) v->loop = 0;
        uint32_t eg1 = ram16(k, p + 8), eg2 = ram16(k, p + 10);
        uint32_t ar = eg1 & 31, d1r = (eg1 >> 6) & 31, d2r = (eg1 >> 11) & 31;
        uint32_t rr = eg2 & 31, dl = (eg2 >> 5) & 31;
        v->eg_ar = ar >= 31 ? -1 : eg_rate_step[2 * ar] * 8;  /* attack about 8x faster than a decay */
        v->eg_d1 = eg_rate_step[2 * d1r];
        v->eg_d2 = eg_rate_step[2 * d2r];
        v->eg_rr = eg_rate_step[2 * rr];
        v->eg_dl = (int32_t)(dl << 5);
        v->eg = v->eg_ar < 0 ? 0 : ATT_MAX << 16;
        v->eg_stage = v->eg_ar < 0 ? EG_DECAY1 : EG_ATTACK;
        uint32_t send = ram16(k, p + 18), tlq = ram16(k, p + 20);
        uint32_t disdl = (send >> 8) & 15, tl = tlq >> 8, dipan = send & 31;
        /* the split's DIPAN on the pan scale, as the driver converts it */
        v->span = (uint8_t)(dipan == 0x10 ? 64 : dipan & 0x10 ? (15 - (dipan & 15)) * 4 : (dipan + 16) * 4);
        v->att_static = disdl ? (int32_t)(15 - disdl) * 32 : ATT_MAX;
        v->tl = (uint8_t)tl;
        v->vlev = (uint8_t)(curve_at(k, bank_curve_n(k, ch->bank, ram8(k, p + 42)), (uint32_t)vel) + 1);
        int32_t base = (int32_t)ram8(k, p + 38), fine = (int8_t)ram8(k, p + 39);
        v->base_cents = (key - base) * 100 + fine;
        v->pitch_reg = (uint16_t)ram16(k, p + 12);
        v->step = pitch_step(v->base_cents, v->pitch_reg);
        v->bend_range = sp[l].bend ? sp[l].bend : 2;
        v->delay = (uint32_t)sp[l].delay * (SND_RATE / 1000);
        v->age = ++k->age;
        v->pos = 0; v->frac = 0; v->ad_next = 0; v->ad_q = 127;
        v->cur = tone_sample(k, v, 0);
        int64_t nx = tone_next(v, 0);
        v->nxt = nx < 0 ? 0 : tone_sample(k, v, (uint32_t)nx);
        v->on = 1;
    }
}

static void note_off(snd_core *k, int port, int c, int key) {
    for (int i = 0; i < SND_VOICES; i++) {
        voice *v = &k->v[i];
        if (v->on && v->port == port && v->ch == c && v->key == key) v->eg_stage = EG_RELEASE;
    }
}

static void chan_reset(chan *ch) { ch->vol = 100; ch->pan = 0xC0; ch->expr = 127; ch->mod = 0; ch->bend = 0; }

static void midi(snd_core *k, int port, int st, int d1, int d2) {
    if (port < 0 || port >= SND_PORTS) return;
    int c = st & 15;
    chan *ch = &k->ch[port][c];
    switch (st & 0xF0) {
    case 0x80: note_off(k, port, c, d1); break;
    case 0x90: if (d2) note_on(k, port, c, d1, d2); else note_off(k, port, c, d1); break;
    case 0xB0:
        switch (d1) {
        case 0: break;                              /* bank MSB: unused */
        case 32: ch->bank = (uint8_t)d2; break;     /* bank LSB = MLT bank number */
        case 1: ch->mod = (uint8_t)d2; break;
        case 7: ch->vol = (uint8_t)d2; break;
        case 10: ch->pan = (uint8_t)d2; break;
        case 11: ch->expr = (uint8_t)d2; break;
        case 120: case 123:                         /* all sound / notes off */
            for (int i = 0; i < SND_VOICES; i++) if (k->v[i].on && k->v[i].port == port && k->v[i].ch == c) k->v[i].eg_stage = EG_RELEASE;
            break;
        case 121: chan_reset(ch); break;
        default: break;
        }
        break;
    case 0xC0: ch->prog = (uint8_t)d1; break;
    case 0xE0: ch->bend = (int16_t)(((d2 & 127) << 7 | (d1 & 127)) - 8192); break;
    default: break;
    }
}

/* Mixes voice v into acc (n stereo pairs). */
/* Advances the envelope by n samples; 0 when the voice has gone silent. */
#define EG_CHUNK 16
static int eg_advance(voice *v, int32_t n) {
    switch (v->eg_stage) {
    case EG_ATTACK: v->eg -= v->eg_ar * n; if (v->eg <= 0) { v->eg = 0; v->eg_stage = EG_DECAY1; } break;
    case EG_DECAY1: v->eg += v->eg_d1 * n; if ((v->eg >> 16) >= v->eg_dl) v->eg_stage = EG_DECAY2; break;
    case EG_DECAY2: v->eg += v->eg_d2 * n; break;
    case EG_RELEASE: v->eg += v->eg_rr * n; break;
    }
    return (v->eg >> 16) < ATT_MAX;
}

/* Mixes voice v into acc (n stereo pairs).  The envelope and the gains
 * step once per EG_CHUNK samples (0.36 ms; the AICA's envelope also moves
 * slower than its sample rate), the sample position every sample.  Every
 * product fits in 32 bits (the PSP has no cheap 64-bit multiply). */
static void voice_render(snd_core *k, voice *v, int32_t *acc, uint32_t n) {
    chan *ch = &k->ch[v->port][v->ch];
    /* no group level (port_att): the console's TL never carries the -6 dB
     * the game sets on the MIDI ports, and the level matches without it */
    int32_t att = v->att_static + tone_att(k, v, ch);
    int pan = ch->pan & 0x80 ? v->span : ch->pan;
    int32_t att_l = att + (pan >= 64 ? pan_att[pan] : 0);
    int32_t att_r = att + (pan < 64 ? pan_att[pan] : 0);
    uint32_t step = v->step;
    if (ch->bend) step = pitch_step(v->base_cents + ch->bend * v->bend_range * 100 / 8192, v->pitch_reg);
    uint32_t i = 0;
    if (v->delay) {
        uint32_t d = v->delay < n ? v->delay : n;
        v->delay -= d; i = d;
    }
    while (i < n) {
        uint32_t m = n - i < EG_CHUNK ? n - i : EG_CHUNK;
        if (!eg_advance(v, (int32_t)m)) { v->on = 0; return; }
        int32_t e = v->eg >> 16;
        int32_t gl = gain_q15[clamp_att(att_l + e)], gr = gain_q15[clamp_att(att_r + e)];
        for (uint32_t j = 0; j < m; j++, i++) {
            int32_t s = v->cur + (((v->nxt - v->cur) * (int32_t)(v->frac >> 1)) >> 15);
            acc[2 * i] += (s * gl) >> 15;
            acc[2 * i + 1] += (s * gr) >> 15;
            v->frac += step;
            while (v->frac >= 0x10000u) {
                v->frac -= 0x10000u;
                int64_t nx = tone_next(v, v->pos);
                if (nx < 0) { v->on = 0; return; }
                v->pos = (uint32_t)nx;
                v->cur = v->nxt;
                int64_t nn = tone_next(v, v->pos);
                v->nxt = nn < 0 ? 0 : tone_sample(k, v, (uint32_t)nn);
            }
        }
    }
}

/* ---- streams ---------------------------------------------------------------- */
static uint32_t ring_avail(const snd_byte_ring *r, uint32_t tail) { return r->head - tail; }

/* The next source sample of every channel into out; 0 at the end, -1 when
 * the producer has not delivered the block yet. */
static int stream_fetch(snd_core *k, int si, int32_t *out) {
    stream *s = &k->st[si];
    adx_info *a = &s->info;
    uint32_t end = a->loop ? a->loop_end : a->samples;
    if (s->s >= end) {
        if (!a->loop) return 0;
        s->s = a->loop_start;
        memcpy(s->hist, s->saved, sizeof s->hist);
        s->blk_valid = 0;
    }
    if (!s->blk_valid || s->s >= s->blk_start + ADX_BLOCK_SAMPLES) {
        snd_byte_ring *r = &k->sh->stream_data[si];
        uint32_t gb = adx_group_bytes(a);
        SND_BARRIER();
        if (ring_avail(r, s->tail) < gb) return -1;
        uint32_t g = adx_group_of(s->s);
        if (a->loop && g == adx_group_of(a->loop_start) && !s->saved_ok) { memcpy(s->saved, s->hist, sizeof s->saved); s->saved_ok = 1; }
        uint8_t tmp[ADX_BLOCK * ADX_MAX_CH];
        for (uint32_t i = 0; i < gb; i++) tmp[i] = r->buf[(s->tail + i) & (SND_STREAM_RING - 1)];
        SND_BARRIER();
        s->tail += gb;
        r->tail = s->tail;
        for (uint32_t c = 0; c < a->channels; c++) adx_decode_block(tmp + ADX_BLOCK * c, a->coef1, a->coef2, &s->hist[c], s->blk[c]);
        s->blk_start = g * ADX_BLOCK_SAMPLES;
        s->blk_valid = 1;
    }
    uint32_t j = s->s - s->blk_start;
    for (uint32_t c = 0; c < a->channels; c++) out[c] = s->blk[c][j];
    s->s++;
    s->decoded++;
    return 1;
}

static void stream_render(snd_core *k, int si, int32_t *acc, uint32_t n) {
    stream *s = &k->st[si];
    if (!s->on) return;
    int32_t g = gain_q15[clamp_att(s->att + (si == 0 ? TRIM_MUSIC_ATT : 0))];
    uint32_t nc = s->info.channels;
    for (uint32_t i = 0; i < n; i++) {
        while (s->frac >= 0x10000u) {
            int32_t x[ADX_MAX_CH];
            int r = stream_fetch(k, si, x);
            if (r < 0) goto out;                     /* underrun: hold, no time passes */
            if (r == 0) { s->on = 0; s->finished = 1; goto out; }
            s->frac -= 0x10000u;
            for (uint32_t c = 0; c < nc; c++) { s->prev[c] = s->cur[c]; s->cur[c] = x[c]; }
        }
        int32_t t = (int32_t)(s->frac >> 1);
        int32_t l = s->prev[0] + (((s->cur[0] - s->prev[0]) * t) >> 15);
        int32_t r = nc > 1 ? s->prev[1] + (((s->cur[1] - s->prev[1]) * t) >> 15) : l;
        acc[2 * i] += (l * g) >> 15;
        acc[2 * i + 1] += (r * g) >> 15;
        s->frac += s->step;
        s->played++;
    }
out:;
    snd_stream_status *st = &k->sh->status[si];
    st->played = s->played;
    st->pos = s->decoded;
    st->state = s->on ? 3 : s->finished ? 5 : 0;
}

static void stream_start(snd_core *k, int si, uint32_t ringpos, uint32_t gen) {
    stream *s = &k->st[si];
    memset(s, 0, sizeof *s);
    s->info = k->sh->stream_info[si];
    s->generation = gen;
    s->tail = ringpos;
    k->sh->stream_data[si].tail = ringpos;
    s->step = (uint32_t)(((uint64_t)s->info.rate << 16) / SND_RATE);
    s->frac = 0x10000u;                              /* fetch the first sample at once */
    s->att = 0;
    s->on = s->info.channels >= 1 && s->info.channels <= ADX_MAX_CH && s->step;
    snd_stream_status *st = &k->sh->status[si];
    st->generation = gen; st->played = 0; st->pos = 0; st->state = s->on ? 3 : 5;
}

/* ---- commands ---------------------------------------------------------------- */
static void run_commands(snd_core *k) {
    snd_cmd_ring *r = &k->sh->cmds;
    SND_BARRIER();
    uint32_t head = r->head;
    while (r->tail != head) {
        snd_cmd c = r->cmd[r->tail & (SND_CMD_RING - 1)];
        SND_BARRIER();
        r->tail++;
        switch (c.op) {
        case SND_CMD_MIDI: midi(k, c.a, c.b, c.c, c.x & 0xFF); break;
        case SND_CMD_MIDI_RESET:
            for (int i = 0; i < SND_VOICES; i++) k->v[i].on = 0;
            for (int p = 0; p < SND_PORTS; p++) for (int i = 0; i < 16; i++) { chan_reset(&k->ch[p][i]); k->ch[p][i].bank = 0; k->ch[p][i].prog = 0; }
            break;
        case SND_CMD_PORT_VOL: if (c.a < 4) k->port_att[c.a] = c.x >= 0 ? 0 : ATT_DB(-c.x / 8.0); break;
        case SND_CMD_STREAM_START: if (c.a < SND_STREAMS) stream_start(k, c.a, (uint32_t)c.x, (uint32_t)c.y); break;
        case SND_CMD_STREAM_STOP:
            if (c.a < SND_STREAMS) {
                stream *s = &k->st[c.a];
                s->on = 0; s->finished = 0;
                k->sh->status[c.a].state = 0;
                s->tail = k->sh->stream_data[c.a].head;     /* drop what was queued */
                k->sh->stream_data[c.a].tail = s->tail;
            }
            break;
        case SND_CMD_STREAM_VOL:
            if (c.a < SND_STREAMS) k->st[c.a].att = c.x >= 0 ? 0 : c.x <= -960 ? ATT_MAX : ATT_DB(-c.x / 10.0);
            break;
        case SND_CMD_BANK_CHANGED:
            for (int i = 0; i < SND_VOICES; i++) {
                voice *v = &k->v[i];
                if (v->on && v->addr >= (uint32_t)c.x && v->addr < (uint32_t)c.x + (uint32_t)c.y) v->on = 0;
            }
            break;
        default: break;
        }
    }
}

/* ---- public ---------------------------------------------------------------- */
uint32_t snd_core_size(void) { return sizeof(snd_core); }

void snd_core_init(snd_core *k, snd_shared *sh) {
    build_tables();
    memset(k, 0, sizeof *k);
    k->sh = sh;
    for (int p = 0; p < SND_PORTS; p++) for (int i = 0; i < 16; i++) chan_reset(&k->ch[p][i]);
}

void snd_core_rebind(snd_core *k, snd_shared *sh) { build_tables(); k->sh = sh; }

void snd_core_render(snd_core *k, int16_t *out, uint32_t n) {
    run_commands(k);
    int32_t acc[2 * SND_FRAME_SAMPLES];
    while (n) {
        uint32_t m = n > SND_FRAME_SAMPLES ? SND_FRAME_SAMPLES : n;
        memset(acc, 0, sizeof(int32_t) * 2 * m);
        uint32_t active = 0;
        uint32_t mute = k->sh->mute;
        static int32_t scratch[2 * SND_FRAME_SAMPLES];
        uint32_t solo = k->sh->solo;
        for (int i = 0; i < SND_VOICES; i++) {
            voice *v = &k->v[i];
            if (!v->on) continue;
            int quiet = (mute & SND_MUTE_FX) || (solo && (v->bank != ((solo >> 8) & 0xFF) || v->prog != (solo & 0xFF)));
            voice_render(k, v, quiet ? scratch : acc, m);
            active++;
        }
        for (int si = 0; si < SND_STREAMS; si++)
            stream_render(k, si, (mute & (si == 0 ? SND_MUTE_MUSIC : SND_MUTE_VOICE)) ? scratch : acc, m);
        for (uint32_t i = 0; i < 2 * m; i++) {
            int32_t x = acc[i];
            out[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        }
        k->sh->voices_active = active;
        k->sh->rendered += m;
        out += 2 * m; n -= m;
    }
}

/* ---- producer side ------------------------------------------------------------ */
int snd_cmd_push(snd_shared *sh, const snd_cmd *c) {
    snd_cmd_ring *r = &sh->cmds;
    if (r->head - r->tail >= SND_CMD_RING) return -1;
    r->cmd[r->head & (SND_CMD_RING - 1)] = *c;
    SND_BARRIER();
    r->head++;
    return 0;
}

uint32_t snd_ring_free(const snd_byte_ring *r) { return SND_STREAM_RING - (r->head - r->tail); }

void snd_ring_write(snd_byte_ring *r, const uint8_t *p, uint32_t n) {
    uint32_t h = r->head;
    for (uint32_t i = 0; i < n; i++) r->buf[(h + i) & (SND_STREAM_RING - 1)] = p[i];
    SND_BARRIER();
    r->head = h + n;
}
