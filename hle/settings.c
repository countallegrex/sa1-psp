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
 *   frame_skip = auto | off | N  auto (default): skip pictures when behind,
 *                              logic runs every frame (hle/game/frame_skip.c;
 *                              never in strict runs); N: draw one frame in N
 *
 * Part of sa1-psp (MIT). */
#include "hle.h"
#include "sound/snd_host.h"
#include <string.h>
#include <ctype.h>

int hle_parse_frame_skip(const char *v, int *force) {
    if (force) *force = 0;
    if (!strncmp(v, "force:", 6)) { if (force) *force = 1; v += 6; }
    if (!strcmp(v, "auto") || !strcmp(v, "on") || !strcmp(v, "1")) return -1;
    if (!strcmp(v, "off") || !strcmp(v, "0")) return 0;
    int n = 0;
    for (const char *p = v; *p; p++) { if (*p < '0' || *p > '9') return -3; n = n * 10 + (*p - '0'); if (n > 60) return -3; }
    return n >= 2 ? n : -3;
}

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
            } else if (!strcmp(k, "frame_skip")) {
                int m = hle_parse_frame_skip(v, NULL);
                if (m != -3) { hle_frame_skip = m; sa1_log("settings: frame_skip = %s", v); }
                else sa1_log("settings: frame_skip = %s: not auto, off or 2-60", v);
            } else if (*k) sa1_log("settings: unknown key \"%s\"", k);
        }
        line = next;
    }
}
