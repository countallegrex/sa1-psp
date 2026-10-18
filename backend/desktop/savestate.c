/* savestate.c - save states (savestate.h, docs/savestate.md).
 *
 * The run's state lives in three places: guest RAM (one flat window at a
 * fixed address), the HLE modules' variables (each module names them to an
 * hle_state_io) and the coroutine stacks (fixed slots; a suspended context
 * is its saved stack pointer plus the bytes above it).  All three are
 * copied while nothing runs on them: the game context that wants a save
 * switches to the supervisor (the OS thread's own stack, never saved), the
 * supervisor writes the file and switches back.  A load is the same in
 * reverse, and ends by switching into whichever context the file says was
 * running, which resumes inside savestate_save's switch and returns to the
 * frame hook of the saved frame.
 *
 * File: "SA1STATE", u32 version, u32 header length, the header (build hash,
 * the address of a static, the fixed base, meta, raw and packed body sizes),
 * then the body packed as byte runs.  The body is a chunk stream
 * (hle/state.c): RAM, the modules' chunks, STACKS, CURRENT.
 *
 * Part of sa1-psp (MIT). */
#include "savestate.h"
#include "platform_desktop.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#define MAGIC "SA1STATE"
#define VERSION 1u

typedef struct header {
    uint8_t exe_sha[32];
    uint64_t anchor;         /* &sa1_ram: where this build's statics are */
    uint64_t base;           /* SA1_FIXED_BASE */
    uint32_t frame, flags;
    int64_t rtc_epoch;
    uint64_t raw_len, packed_len;
} header;

#define MODULE_MAX 16
static struct { const char *name; void (*fn)(hle_state_io *); } modules[MODULE_MAX];
static int nmodules;
static void (*meta_fill)(savestate_meta *);
static void (*meta_loaded)(const savestate_meta *);

void savestate_add(const char *name, void (*fn)(hle_state_io *io)) {
    if (nmodules < MODULE_MAX) { modules[nmodules].name = name; modules[nmodules].fn = fn; nmodules++; }
    else sa1_log("savestate: module table full, %s dropped", name);
}
void savestate_set_meta(void (*fill)(savestate_meta *), void (*loaded)(const savestate_meta *)) { meta_fill = fill; meta_loaded = loaded; }

/* ---- byte runs ------------------------------------------------------------
 * ops: 0x00 n = n zero bytes; 0x01 n <n bytes> literal; 0x02 n b = n copies
 * of b; n is a LEB128 varint.  Guest RAM is mostly zero or long runs. */
static size_t varint_put(uint8_t *p, uint64_t v) {
    size_t i = 0;
    do { uint8_t b = v & 0x7F; v >>= 7; p[i++] = (uint8_t)(b | (v ? 0x80 : 0)); } while (v);
    return i;
}
static int varint_get(const uint8_t *p, size_t n, size_t *pos, uint64_t *v) {
    uint64_t r = 0; int shift = 0;
    while (*pos < n && shift < 64) {
        uint8_t b = p[(*pos)++];
        r |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *v = r; return 0; }
        shift += 7;
    }
    return -1;
}

uint8_t *savestate_pack(const uint8_t *in, size_t n, size_t *out_n) {
    size_t cap = n + n / 64 + 64, o = 0;
    uint8_t *out = malloc(cap);
    if (!out) return NULL;
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && in[i + run] == in[i]) run++;
        if (run >= 4) {
            out[o++] = in[i] ? 2 : 0;
            o += varint_put(out + o, run);
            if (in[i]) out[o++] = in[i];
            i += run;
            continue;
        }
        /* literal: up to the next run of 4 or more */
        size_t j = i + 1;
        while (j < n) {
            size_t r = 1;
            while (j + r < n && r < 4 && in[j + r] == in[j]) r++;
            if (r >= 4) break;
            j++;
        }
        size_t len = j - i;
        if (o + len + 16 > cap) { cap = cap * 2 + len; uint8_t *b = realloc(out, cap); if (!b) { free(out); return NULL; } out = b; }
        out[o++] = 1;
        o += varint_put(out + o, len);
        memcpy(out + o, in + i, len); o += len;
        i = j;
    }
    *out_n = o;
    return out;
}

int savestate_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t out_n) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t op = in[i++];
        uint64_t len;
        if (varint_get(in, n, &i, &len) != 0 || o + len > out_n) return -1;
        if (op == 0) memset(out + o, 0, (size_t)len);
        else if (op == 1) { if (i + len > n) return -1; memcpy(out + o, in + i, (size_t)len); i += (size_t)len; }
        else if (op == 2) { if (i >= n) return -1; memset(out + o, in[i++], (size_t)len); }
        else return -1;
        o += (size_t)len;
    }
    return o == out_n ? 0 : -1;
}

/* ---- this build ------------------------------------------------------------- */
static int exe_hash(uint8_t out[32]) {
    static uint8_t cached[32]; static int have;
    if (have) { memcpy(out, cached, 32); return 0; }
    char path[2048];
#ifdef _WIN32
    if (!GetModuleFileNameA(NULL, path, sizeof path)) return -1;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return -1;
    path[n] = 0;
#endif
    if (sha256_file(path, cached) != 0) return -1;
    have = 1;
    memcpy(out, cached, 32);
    return 0;
}

/* ---- the body ------------------------------------------------------------------ */
typedef struct stack_rec { uint32_t index; uint64_t sp, hi; } stack_rec;

static int body_write(hle_state_io *io, int current) {
    hle_state_blob(io, "RAM", sa1_ram, SA1_RAM_ALLOC);
    for (int i = 0; i < nmodules; i++) modules[i].fn(io);
    uint32_t count = 0;
    for (int i = 0; i < SA1_CORO_SLOTS; i++) count += sa1_plat_coro_used(i) != 0;
    hle_state_blob(io, "STACKS", &count, 4);
    for (int i = 0; i < SA1_CORO_SLOTS; i++) {
        if (!sa1_plat_coro_used(i)) continue;
        uintptr_t lo, hi, sp = sa1_plat_coro_sp(i);
        sa1_plat_coro_stack(i, &lo, &hi);
        if (sp < lo || sp > hi) { sa1_log("savestate: context %d has stack pointer 0x%llx outside its stack", i, (unsigned long long)sp); return -1; }
        stack_rec r = { (uint32_t)i, sp, hi };
        hle_state_blob(io, "stack", &r, sizeof r);
        uint32_t n = (uint32_t)(hi - sp);
        hle_state_var(io, "stack.bytes", (const void *)sp, &n);
    }
    int32_t cur = current;
    hle_state_blob(io, "CURRENT", &cur, 4);
    return hle_state_error(io) ? -1 : 0;
}

/* Reads the body into place.  Guest RAM, the modules and the stacks are
 * overwritten as they are read; a failure part way leaves a run that must
 * not continue, which the callers treat as fatal. */
static int body_read(hle_state_io *io, int *current) {
    hle_state_blob(io, "RAM", sa1_ram, SA1_RAM_ALLOC);
    for (int i = 0; i < nmodules && !hle_state_error(io); i++) modules[i].fn(io);
    uint32_t count = 0;
    hle_state_blob(io, "STACKS", &count, 4);
    if (hle_state_error(io)) return -1;
    sa1_plat_coro_reset_all();
    for (uint32_t k = 0; k < count; k++) {
        stack_rec r;
        hle_state_blob(io, "stack", &r, sizeof r);
        uint32_t n = 0;
        const void *bytes = hle_state_var(io, "stack.bytes", NULL, &n);
        if (hle_state_error(io)) return -1;
        uintptr_t lo, hi;
        sa1_plat_coro_stack((int)r.index, &lo, &hi);
        if (!lo || r.hi != hi || r.sp < lo || r.sp > hi || n != hi - r.sp) {
            sa1_log("savestate: stack record %u (context %u, sp 0x%llx, %u bytes) does not fit this build's slot", k, r.index, (unsigned long long)r.sp, n);
            return -1;
        }
        memcpy((void *)(uintptr_t)r.sp, bytes, n);
        sa1_plat_coro_restore((int)r.index, (uintptr_t)r.sp);
    }
    int32_t cur = -1;
    hle_state_blob(io, "CURRENT", &cur, 4);
    if (hle_state_error(io)) return -1;
    if (!sa1_plat_coro_used(cur)) { sa1_log("savestate: the file's current context %d was not saved", cur); return -1; }
    *current = cur;
    return 0;
}

/* ---- files ---------------------------------------------------------------------- */
static int do_save(const char *path, int current) {
    header h;
    memset(&h, 0, sizeof h);
    if (exe_hash(h.exe_sha) != 0) { sa1_log("savestate: cannot hash the executable"); return -1; }
    h.anchor = (uint64_t)(uintptr_t)&sa1_ram;
    h.base = SA1_FIXED_BASE;
    savestate_meta m = {0};
    if (meta_fill) meta_fill(&m);
    h.frame = m.frame; h.flags = m.flags; h.rtc_epoch = m.rtc_epoch;

    hle_state_io *io = hle_state_writer();
    if (!io) return -1;
    int r = body_write(io, current);
    if (r != 0) { sa1_log("savestate: %s", hle_state_error(io) ? hle_state_error(io) : "cannot collect the state"); hle_state_free(io); return -1; }
    size_t raw_n;
    const uint8_t *raw = hle_state_bytes(io, &raw_n);
    size_t packed_n;
    uint8_t *packed = savestate_pack(raw, raw_n, &packed_n);
    hle_state_free(io);
    if (!packed) { sa1_log("savestate: out of memory"); return -1; }
    h.raw_len = raw_n; h.packed_len = packed_n;

    FILE *f = fopen(path, "wb");
    if (!f) { sa1_log("savestate: cannot write %s", path); free(packed); return -1; }
    uint32_t ver = VERSION, hlen = (uint32_t)sizeof h;
    int ok = fwrite(MAGIC, 1, 8, f) == 8 && fwrite(&ver, 4, 1, f) == 1 && fwrite(&hlen, 4, 1, f) == 1 &&
             fwrite(&h, sizeof h, 1, f) == 1 && fwrite(packed, 1, packed_n, f) == packed_n;
    if (fclose(f) != 0) ok = 0;
    free(packed);
    if (!ok) { sa1_log("savestate: write error on %s", path); return -1; }
    sa1_log("savestate: saved frame %u to %s (%zu bytes packed from %zu; context %d running)", m.frame, path, packed_n + 16 + sizeof h, raw_n, current);
    return 0;
}

static int read_file(const char *path, uint8_t **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { sa1_log("savestate: cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); sa1_log("savestate: cannot read %s", path); return -1; }
    fclose(f);
    *out = buf; *n = (size_t)len;
    return 0;
}

static int do_load(const char *path, int *current, savestate_meta *m) {
    uint8_t *file; size_t n;
    if (read_file(path, &file, &n) != 0) return -1;
    header h;
    uint32_t ver = 0, hlen = 0;
    if (n < 16 + sizeof h || memcmp(file, MAGIC, 8) != 0) { sa1_log("savestate: %s is not a state file", path); free(file); return -1; }
    memcpy(&ver, file + 8, 4); memcpy(&hlen, file + 12, 4);
    if (ver != VERSION || hlen != sizeof h) { sa1_log("savestate: %s is version %u with a %u-byte header; this build writes %u/%u", path, ver, hlen, VERSION, (unsigned)sizeof h); free(file); return -1; }
    memcpy(&h, file + 16, sizeof h);
    uint8_t sha[32];
    if (exe_hash(sha) != 0 || memcmp(sha, h.exe_sha, 32) != 0) {
        char a[65], b[65]; sha256_hex(h.exe_sha, a); sha256_hex(sha, b);
        sa1_log("savestate: %s was saved by another build (executable %.16s..., this one %.16s...); a state holds native stacks and belongs to one build", path, a, b);
        free(file); return -1;
    }
    if (h.anchor != (uint64_t)(uintptr_t)&sa1_ram || h.base != SA1_FIXED_BASE) {
        sa1_log("savestate: this process is laid out differently (statics at 0x%llx, file 0x%llx): the executable must be linked without ASLR", (unsigned long long)(uintptr_t)&sa1_ram, (unsigned long long)h.anchor);
        free(file); return -1;
    }
    if (16 + sizeof h + h.packed_len != n) { sa1_log("savestate: %s is truncated", path); free(file); return -1; }
    uint8_t *raw = malloc((size_t)h.raw_len);
    if (!raw) { free(file); return -1; }
    if (savestate_unpack(file + 16 + sizeof h, (size_t)h.packed_len, raw, (size_t)h.raw_len) != 0) { sa1_log("savestate: %s: packed body is corrupt", path); free(raw); free(file); return -1; }
    free(file);
    hle_state_io *io = hle_state_reader(raw, (size_t)h.raw_len);
    int r = io ? body_read(io, current) : -1;
    if (r != 0 || !hle_state_at_end(io)) {
        sa1_log("savestate: %s: %s", path, io && hle_state_error(io) ? hle_state_error(io) : r == 0 ? "trailing data" : "cannot restore");
        r = -1;
    }
    hle_state_free(io);
    free(raw);
    if (r == 0) {
        m->frame = h.frame; m->flags = h.flags; m->rtc_epoch = h.rtc_epoch;
        sa1_log("savestate: loaded %s: frame %u, context %d resumes", path, h.frame, *current);
    }
    return r;
}

/* ---- the supervisor ------------------------------------------------------------ */
enum { REQ_NONE, REQ_SAVE, REQ_LOAD };
static int req_kind, req_result;
static const char *req_path;
static sa1_coro *req_from;

int savestate_supervise(void (*boot)(void *arg), void *arg, const char *load_path) {
    sa1_coro *sup = sa1_plat_coro_supervisor();
    sa1_coro *resume;
    if (load_path) {
        int cur; savestate_meta m = {0};
        if (do_load(load_path, &cur, &m) != 0) return -1;
        if (meta_loaded) meta_loaded(&m);
        resume = sa1_plat_coro_by_index(cur);
    } else {
        resume = sa1_plat_coro_create(boot, arg, SA1_CORO_STACK);
        if (!resume) { sa1_log("savestate: cannot create the root context"); return -1; }
    }
    for (;;) {
        req_kind = REQ_NONE;
        sa1_plat_coro_switch(resume);
        if (req_kind == REQ_SAVE) {
            req_result = do_save(req_path, sa1_plat_coro_index(req_from));
            resume = req_from;
        } else if (req_kind == REQ_LOAD) {
            int cur; savestate_meta m = {0};
            if (do_load(req_path, &cur, &m) == 0) {
                if (meta_loaded) meta_loaded(&m);
                resume = sa1_plat_coro_by_index(cur);
            } else {
                /* the load may have overwritten guest RAM or stacks before failing */
                sa1_log("savestate: load of %s failed; the run cannot continue", req_path);
                return -1;
            }
        } else {
            return 0;        /* the game context came back on its own: boot returned */
        }
    }
    (void)sup;
}

static int request(int kind, const char *path) {
    sa1_coro *me = sa1_plat_coro_current();
    if (me == sa1_plat_coro_supervisor()) { sa1_log("savestate: %s requested outside a game context", kind == REQ_SAVE ? "save" : "load"); return -1; }
    req_kind = kind; req_path = path; req_from = me; req_result = -1;
    sa1_plat_coro_switch(sa1_plat_coro_supervisor());
    /* a save returns here; a load resumes the file's context instead */
    return req_result;
}
int savestate_save(const char *path) { return request(REQ_SAVE, path); }
int savestate_load(const char *path) { return request(REQ_LOAD, path); }
