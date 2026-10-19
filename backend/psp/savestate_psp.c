/* savestate_psp.c - save states on the PSP: backend/desktop/savestate.h's
 * interface and the desktop's design (docs/savestate.md): guest RAM, the
 * HLE modules' chunks and the coroutine stacks, written by the supervisor
 * (the PSP main thread, never saved) while every game context is suspended,
 * and a load that resumes whichever context the file says was running.
 *
 * What differs from backend/desktop/savestate.c:
 *  - The PSP has no room for a whole state in memory (20 MB of guest RAM
 *    alone, against 3.5-11 MB free), so the chunks stream to and from the
 *    file (hle_state_stream_*), unpacked: a state is ~30 MB.
 *  - A state holds native stacks, so it loads only into the build and the
 *    memory layout that saved it: the header keeps the hash of the running
 *    image (the PRX or EBOOT file, savestate_set_image), &sa1_ram (where the
 *    image's statics are: the PRX's load address), sa1_ram itself (the
 *    heap) and the coroutine region (the main thread's stack).  Under
 *    PSPLink these repeat from run to run of a build; a desktop state never
 *    loads here (its stacks are x86).
 *
 * File: "SA1STATP", u32 version, u32 header length, the header, the body:
 * the chunk stream RAM, the modules, STACKS, CURRENT.
 *
 * Part of sa1-psp (MIT). */
#include "../desktop/savestate.h"
#include "../desktop/sha256.h"
#include "platform_psp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "SA1STATP"
#define VERSION 1u

typedef struct header {
    uint8_t image_sha[32];
    uint32_t statics, ram, region, region_bytes;
    uint32_t frame, flags;
    int64_t rtc_epoch;
} header;

#define MODULE_MAX 16
static struct { const char *name; void (*fn)(hle_state_io *); } modules[MODULE_MAX];
static int nmodules;
static void (*meta_fill)(savestate_meta *);
static void (*meta_loaded)(const savestate_meta *);
static char image_path[256];

void savestate_add(const char *name, void (*fn)(hle_state_io *io)) {
    if (nmodules < MODULE_MAX) { modules[nmodules].name = name; modules[nmodules].fn = fn; nmodules++; }
    else sa1_log("savestate: module table full, %s dropped", name);
}
void savestate_set_meta(void (*fill)(savestate_meta *), void (*loaded)(const savestate_meta *)) { meta_fill = fill; meta_loaded = loaded; }
void savestate_set_image(const char *path) { snprintf(image_path, sizeof image_path, "%s", path); }

static int image_hash(uint8_t out[32]) {
    static uint8_t cached[32]; static int have;
    if (!have) {
        if (!image_path[0] || sha256_file(image_path, cached) != 0) { sa1_log("savestate: cannot hash the image %s", image_path); return -1; }
        have = 1;
    }
    memcpy(out, cached, 32);
    return 0;
}

static void layout(header *h) {
    uintptr_t lo, hi;
    sa1_plat_coro_stack(0, &lo, &hi);
    h->statics = (uint32_t)(uintptr_t)&sa1_ram;
    h->ram = (uint32_t)(uintptr_t)sa1_ram;
    h->region = (uint32_t)lo;
    h->region_bytes = SA1_CORO_REGION_BYTES;
}

/* ---- the body ------------------------------------------------------------------ */
typedef struct stack_rec { uint32_t index, sp, hi; } stack_rec;

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
        if (sp < lo || sp > hi) { sa1_log("savestate: context %d has stack pointer 0x%08x outside its stack", i, (unsigned)sp); return -1; }
        stack_rec r = { (uint32_t)i, (uint32_t)sp, (uint32_t)hi };
        hle_state_blob(io, "stack", &r, sizeof r);
        uint32_t n = (uint32_t)(hi - sp);
        hle_state_var(io, "stack.bytes", (const void *)sp, &n);
    }
    int32_t cur = current;
    hle_state_blob(io, "CURRENT", &cur, 4);
    return hle_state_error(io) ? -1 : 0;
}

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
            sa1_log("savestate: stack record %u (context %u, sp 0x%08x, %u bytes) does not fit this run's slot", k, r.index, r.sp, n);
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
    if (image_hash(h.image_sha) != 0) return -1;
    layout(&h);
    savestate_meta m = {0};
    if (meta_fill) meta_fill(&m);
    h.frame = m.frame; h.flags = m.flags; h.rtc_epoch = m.rtc_epoch;
    uint64_t t0 = sa1_plat_time_us();
    FILE *f = fopen(path, "wb");
    if (!f) { sa1_log("savestate: cannot write %s", path); return -1; }
    static char fbuf[64 * 1024];
    setvbuf(f, fbuf, _IOFBF, sizeof fbuf);
    uint32_t ver = VERSION, hlen = (uint32_t)sizeof h;
    int ok = fwrite(MAGIC, 1, 8, f) == 8 && fwrite(&ver, 4, 1, f) == 1 && fwrite(&hlen, 4, 1, f) == 1 && fwrite(&h, sizeof h, 1, f) == 1;
    hle_state_io *io = ok ? hle_state_stream_writer(f) : NULL;
    int r = io ? body_write(io, current) : -1;
    if (io && hle_state_error(io)) sa1_log("savestate: %s", hle_state_error(io));
    size_t n = io ? hle_state_stream_written(io) : 0;
    hle_state_free(io);
    if (fclose(f) != 0) ok = 0;
    if (!ok || r != 0) { sa1_log("savestate: cannot write %s", path); return -1; }
    sa1_log("savestate: saved frame %u to %s (%u KB, %.1f s; context %d running)", m.frame, path, (unsigned)(n / 1024u),
            (sa1_plat_time_us() - t0) / 1e6, current);
    return 0;
}

static int do_load(const char *path, int *current, savestate_meta *m) {
    uint64_t t0 = sa1_plat_time_us();
    FILE *f = fopen(path, "rb");
    if (!f) { sa1_log("savestate: cannot open %s", path); return -1; }
    static char fbuf[64 * 1024];
    setvbuf(f, fbuf, _IOFBF, sizeof fbuf);
    header h, now;
    char magic[8];
    uint32_t ver = 0, hlen = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, MAGIC, 8) != 0 || fread(&ver, 4, 1, f) != 1 || fread(&hlen, 4, 1, f) != 1) {
        sa1_log("savestate: %s is not a PSP state file (a desktop state does not load here)", path); fclose(f); return -1;
    }
    if (ver != VERSION || hlen != sizeof h || fread(&h, sizeof h, 1, f) != 1) { sa1_log("savestate: %s is version %u/%u, this build %u/%u", path, ver, hlen, VERSION, (unsigned)sizeof h); fclose(f); return -1; }
    memset(&now, 0, sizeof now);
    if (image_hash(now.image_sha) != 0) { fclose(f); return -1; }
    layout(&now);
    if (memcmp(now.image_sha, h.image_sha, 32) != 0) {
        char a[65], b[65]; sha256_hex(h.image_sha, a); sha256_hex(now.image_sha, b);
        sa1_log("savestate: %s was saved by another build (%.16s..., this one %.16s...): a state holds native stacks", path, a, b);
        fclose(f); return -1;
    }
    if (now.statics != h.statics || now.ram != h.ram || now.region != h.region || now.region_bytes != h.region_bytes) {
        sa1_log("savestate: this run is laid out differently (statics 0x%08x/0x%08x, RAM 0x%08x/0x%08x, stacks 0x%08x/0x%08x: file/now)",
                h.statics, now.statics, h.ram, now.ram, h.region, now.region);
        fclose(f); return -1;
    }
    hle_state_io *io = hle_state_stream_reader(f);
    int r = io ? body_read(io, current) : -1;
    if (r != 0 || !hle_state_at_end(io)) {
        sa1_log("savestate: %s: %s", path, io && hle_state_error(io) ? hle_state_error(io) : r == 0 ? "trailing data" : "cannot restore");
        r = -1;
    }
    hle_state_free(io);
    fclose(f);
    if (r == 0) {
        m->frame = h.frame; m->flags = h.flags; m->rtc_epoch = h.rtc_epoch;
        sa1_log("savestate: loaded %s in %.1f s: frame %u, context %d resumes", path, (sa1_plat_time_us() - t0) / 1e6, h.frame, *current);
    }
    return r;
}

/* ---- the supervisor (as the desktop's) ------------------------------------------ */
enum { REQ_NONE, REQ_SAVE, REQ_LOAD };
static int req_kind, req_result;
static const char *req_path;
static sa1_coro *req_from;

int savestate_supervise(void (*boot)(void *arg), void *arg, const char *load_path) {
    sa1_coro *resume;
    if (load_path) {
        int cur; savestate_meta m = {0};
        if (do_load(load_path, &cur, &m) != 0) return -1;
        if (meta_loaded) meta_loaded(&m);
        resume = sa1_plat_coro_by_index(cur);
    } else {
        resume = sa1_plat_coro_create(boot, arg, SA1_CORO_ROOT_STACK);
        if (!resume || sa1_plat_coro_index(resume) != 0) { sa1_log("savestate: the root context must be slot 0"); return -1; }
    }
    for (;;) {
        req_kind = REQ_NONE;
        sa1_plat_coro_switch(resume);
        if (req_kind == REQ_SAVE) {
            req_result = do_save(req_path, sa1_plat_coro_index(req_from));
            resume = req_from;
        } else if (req_kind == REQ_LOAD) {
            int cur; savestate_meta m = {0};
            if (do_load(req_path, &cur, &m) != 0) {
                sa1_log("savestate: load of %s failed; the run cannot continue", req_path);
                return -1;
            }
            if (meta_loaded) meta_loaded(&m);
            resume = sa1_plat_coro_by_index(cur);
        } else {
            return 0;
        }
    }
}

static int request(int kind, const char *path) {
    sa1_coro *me = sa1_plat_coro_current();
    if (me == sa1_plat_coro_supervisor()) { sa1_log("savestate: %s requested outside a game context", kind == REQ_SAVE ? "save" : "load"); return -1; }
    req_kind = kind; req_path = path; req_from = me; req_result = -1;
    sa1_plat_coro_switch(sa1_plat_coro_supervisor());
    return req_result;
}
int savestate_save(const char *path) { return request(REQ_SAVE, path); }
int savestate_load(const char *path) { return request(REQ_LOAD, path); }
