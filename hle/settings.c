/* settings.c - the user's settings file: sa1psp.ini in the data root (next
 * to SONICADV/, so the same file works on the desktop and on the Memory
 * Stick).  Lines are "key = value"; '#' or ';' start a comment.  Read once
 * by hle_init; a backend's command line is applied after it and wins.
 *
 *   voice = en | ja | game     voice language (hle/sound: the game's choice
 *                              by default, English on the US disc)
 *   audio = on | off           off: no decoding or mixing (profiling); the
 *                              game behaves the same, voice lines still
 *                              take their length
 *   event_skip = on | off      on (default): START skips a story event
 *                              (hle/game/event_skip.c; never in strict runs)
 *   land_cull = on | opaque | off  on (default): the land table's models wholly
 *                              out of view are not drawn (the picture and the
 *                              game are the same; hle/ninja/native_land.c)
 *   renderer = ta | native     native: the land table from converted meshes
 *                              with hardware transform (hle/ninja/native_land.c;
 *                              off by default, never in strict runs)
 *   frame_skip = auto | off | N  auto (default): skip pictures when behind,
 *                              logic runs every frame (hle/game/frame_skip.c;
 *                              never in strict runs); N: draw one frame in N
 *   math = native | exact      native (default): game routines with host or
 *                              VFPU math (the land collision sweep), the game
 *                              the same within a band; exact: the console's
 *                              operations bit for bit (strict runs, unless
 *                              --math); check: exact plus a native shadow
 *   math_exact = 8c029400,...  these routines stay exact whatever math says
 *                              (A/B timing of one routine; --math-exact)
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"
#include "ninja/ninja.h"
#include "sound/snd_host.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

int hle_parse_frame_skip(const char *v, int *force) {
    if (force) *force = 0;
    if (!strncmp(v, "force:", 6)) { if (force) *force = 1; v += 6; }
    if (!strcmp(v, "auto") || !strcmp(v, "on") || !strcmp(v, "1")) return -1;
    if (!strcmp(v, "off") || !strcmp(v, "0")) return 0;
    int n = 0;
    for (const char *p = v; *p; p++) { if (*p < '0' || *p > '9') return -3; n = n * 10 + (*p - '0'); if (n > 60) return -3; }
    return n >= 2 ? n : -3;
}

int hle_math = 1, hle_math_cli = -1;
int hle_parse_math(const char *v) {
    if (!strcmp(v, "exact")) return 0;
    if (!strcmp(v, "native")) return 1;
    if (!strcmp(v, "check")) return 2;
    return -1;
}
int hle_math_mode(void) {
    if (hle_math_cli >= 0) return hle_math_cli;
    return hle_strict ? 0 : hle_math;
}
static uint32_t math_exact[8];
static int math_exact_n;
void hle_parse_math_exact(const char *v) {
    math_exact_n = 0;
    while (*v && math_exact_n < 8) {
        char *e;
        const unsigned long a = strtoul(v, &e, 16);
        if (e == v) break;
        math_exact[math_exact_n++] = (uint32_t)a | 0x80000000u;
        v = *e ? e + 1 : e;
    }
    sa1_log("settings: math_exact %d routine(s)", math_exact_n);
}
int hle_math_mode_for(uint32_t addr) {
    for (int i = 0; i < math_exact_n; i++) if (math_exact[i] == (addr | 0x80000000u)) return 0;
    return hle_math_mode();
}
void sph_check_report(void);
void pr_check_report(void);
void hle_math_report(void) { sph_check_report(); pr_check_report(); }

int hle_parse_voice(const char *v) {
    if (!strcmp(v, "en") || !strcmp(v, "english") || !strcmp(v, "us")) return 1;
    if (!strcmp(v, "ja") || !strcmp(v, "jp") || !strcmp(v, "japanese")) return 0;
    if (!strcmp(v, "game") || !strcmp(v, "default")) return -1;
    return -2;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

void hle_settings_load(void) {
    uint32_t size;
    if (sa1_plat_file_size("sa1psp.ini", &size) != 0 || size == 0 || size > 16384) return;
    char buf[16385];
    int n = sa1_plat_file_read("sa1psp.ini", 0, buf, size);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = buf; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *hash = strpbrk(line, "#;");
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char *k = trim(line), *v = trim(eq + 1);
            for (char *p = v; *p; p++) *p = (char)tolower((unsigned char)*p);
            if (!strcmp(k, "voice")) {
                int lang = hle_parse_voice(v);
                if (lang >= -1) { snd_voice_lang = lang; sa1_log("settings: voice = %s", v); }
                else sa1_log("settings: voice = %s: not en, ja or game", v);
            } else if (!strcmp(k, "audio")) {
                snd_audio_enabled = strcmp(v, "off") != 0 && strcmp(v, "0") != 0;
                sa1_log("settings: audio %s", snd_audio_enabled ? "on" : "off");
            } else if (!strcmp(k, "event_skip")) {
                hle_event_skip = strcmp(v, "off") != 0 && strcmp(v, "0") != 0;
                sa1_log("settings: event_skip %s", hle_event_skip ? "on" : "off");
            } else if (!strcmp(k, "renderer")) {
                hle_renderer_native = !strcmp(v, "native") ? 1 : !strcmp(v, "noland") ? 2 : 0;
                sa1_log("settings: renderer %s", hle_renderer_native == 1 ? "native" : hle_renderer_native == 2 ? "noland" : "ta");
            } else if (!strcmp(k, "math")) {
                int m = hle_parse_math(v);
                if (m >= 0) { hle_math = m; sa1_log("settings: math %s", v); }
                else sa1_log("settings: math = %s: not native, exact or check", v);
            } else if (!strcmp(k, "math_exact")) {
                hle_parse_math_exact(v);
            } else if (!strcmp(k, "land_cull")) {
                hle_land_cull = hle_parse_land_cull(v);
                sa1_log("settings: land_cull %s", hle_land_cull == 2 ? "opaque" : hle_land_cull ? "on" : "off");
            } else if (!strcmp(k, "frame_skip")) {
                int m = hle_parse_frame_skip(v, NULL);
                if (m != -3) { hle_frame_skip = m; sa1_log("settings: frame_skip = %s", v); }
                else sa1_log("settings: frame_skip = %s: not auto, off or 2-60", v);
            } else if (*k) sa1_log("settings: unknown key \"%s\"", k);
        }
        line = next;
    }
}
