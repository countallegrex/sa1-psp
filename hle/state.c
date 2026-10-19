/* state.c - the chunk writer and reader behind hle/state.h.
 *
 * Part of sa1-psp (MIT). */
#include "state.h"
#include "sh4_rt.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define NAME_LEN 16

struct hle_state_io {
    int loading;
    uint8_t *buf;          /* writer: owned; reader: borrowed */
    size_t len, cap, pos;
    char error[160];
    FILE *f;               /* stream mode (hle_state_stream_*): chunks go to / come from f */
    uint8_t *ring[2];      /* stream reader: the last two hle_state_var payloads */
    uint32_t ring_cap[2];
    int ring_next;
};

int hle_state_loading(const hle_state_io *io) { return io->loading; }
const char *hle_state_error(const hle_state_io *io) { return io->error[0] ? io->error : NULL; }
int hle_state_at_end(const hle_state_io *io) {
    if (io->f) { int c = fgetc(io->f); if (c == EOF) return 1; ungetc(c, io->f); return 0; }
    return io->pos == io->len;
}

hle_state_io *hle_state_stream_writer(FILE *f) {
    hle_state_io *io = calloc(1, sizeof *io);
    if (io) io->f = f;
    return io;
}

hle_state_io *hle_state_stream_reader(FILE *f) {
    hle_state_io *io = calloc(1, sizeof *io);
    if (io) { io->loading = 1; io->f = f; }
    return io;
}

size_t hle_state_stream_written(const hle_state_io *io) { return io->len; }

hle_state_io *hle_state_writer(void) {
    hle_state_io *io = calloc(1, sizeof *io);
    return io;
}

hle_state_io *hle_state_reader(const void *buf, size_t n) {
    hle_state_io *io = calloc(1, sizeof *io);
    if (!io) return NULL;
    io->loading = 1; io->buf = (uint8_t *)buf; io->len = n;
    return io;
}

void hle_state_free(hle_state_io *io) {
    if (!io) return;
    if (!io->loading) free(io->buf);
    free(io->ring[0]); free(io->ring[1]);
    free(io);
}

const void *hle_state_bytes(const hle_state_io *io, size_t *n) { *n = io->len; return io->buf; }

static int fail(hle_state_io *io, const char *fmt, const char *name, uint32_t a, uint32_t b) {
    if (!io->error[0]) snprintf(io->error, sizeof io->error, fmt, name, a, b);
    return -1;
}

static int put(hle_state_io *io, const void *p, size_t n) {
    if (io->f) {
        if (fwrite(p, 1, n, io->f) != n) return fail(io, "write error writing %s (%u bytes)", "state", (uint32_t)n, 0);
        io->len += n;
        return 0;
    }
    if (io->len + n > io->cap) {
        size_t cap = io->cap ? io->cap : 1 << 16;
        while (cap < io->len + n) cap *= 2;
        uint8_t *b = realloc(io->buf, cap);
        if (!b) return fail(io, "out of memory writing %s (%u bytes)", "state", (uint32_t)n, 0);
        io->buf = b; io->cap = cap;
    }
    memcpy(io->buf + io->len, p, n);
    io->len += n;
    return 0;
}

static int write_chunk(hle_state_io *io, const char *name, const void *p, uint32_t n) {
    char tag[NAME_LEN] = {0};
    strncpy(tag, name, NAME_LEN - 1);
    if (put(io, tag, NAME_LEN) || put(io, &n, 4) || put(io, p, n)) return -1;
    return 0;
}

/* Stream reader: the next chunk's header, checked against name; its length
 * in *n.  The payload is next in the file. */
static int stream_header(hle_state_io *io, const char *name, uint32_t *n) {
    if (io->error[0]) return -1;
    char tag[NAME_LEN + 1];
    uint32_t len;
    if (fread(tag, 1, NAME_LEN, io->f) != NAME_LEN || fread(&len, 4, 1, io->f) != 1) return fail(io, "state ends before chunk %s", name, 0, 0);
    tag[NAME_LEN] = 0;
    if (strncmp(tag, name, NAME_LEN - 1) != 0) {
        char both[NAME_LEN * 2 + 8];
        snprintf(both, sizeof both, "%s (file has %s)", name, tag);
        return fail(io, "state chunk mismatch: this build expects %s", both, 0, 0);
    }
    *n = len;
    return 0;
}

/* Reads the next chunk header; returns its payload, or NULL with the error set. */
static const uint8_t *read_chunk(hle_state_io *io, const char *name, uint32_t *n) {
    if (io->error[0]) return NULL;
    if (io->pos + NAME_LEN + 4 > io->len) { fail(io, "state ends before chunk %s", name, 0, 0); return NULL; }
    char tag[NAME_LEN + 1];
    memcpy(tag, io->buf + io->pos, NAME_LEN); tag[NAME_LEN] = 0;
    uint32_t len; memcpy(&len, io->buf + io->pos + NAME_LEN, 4);
    if (strncmp(tag, name, NAME_LEN - 1) != 0) {
        char both[NAME_LEN * 2 + 8];
        snprintf(both, sizeof both, "%s (file has %s)", name, tag);
        fail(io, "state chunk mismatch: this build expects %s", both, 0, 0);
        return NULL;
    }
    if (io->pos + NAME_LEN + 4 + len > io->len) { fail(io, "state chunk %s runs past the end (%u bytes)", name, len, 0); return NULL; }
    const uint8_t *p = io->buf + io->pos + NAME_LEN + 4;
    io->pos += NAME_LEN + 4 + len;
    *n = len;
    return p;
}

void hle_state_blob(hle_state_io *io, const char *name, void *p, uint32_t n) {
    if (!io->loading) { write_chunk(io, name, p, n); return; }
    if (io->f) {
        uint32_t len;
        if (stream_header(io, name, &len) != 0) return;
        if (len != n) { fail(io, "state chunk %s is %u bytes, this build expects %u", name, len, n); return; }
        if (fread(p, 1, n, io->f) != n) fail(io, "state ends inside chunk %s (%u bytes)", name, n, 0);
        return;
    }
    uint32_t len;
    const uint8_t *src = read_chunk(io, name, &len);
    if (!src) return;
    if (len != n) { fail(io, "state chunk %s is %u bytes, this build expects %u", name, len, n); return; }
    memcpy(p, src, n);
}

const void *hle_state_var(hle_state_io *io, const char *name, const void *p, uint32_t *n) {
    if (!io->loading) { write_chunk(io, name, p, *n); return p; }
    if (io->f) {
        uint32_t len;
        if (stream_header(io, name, &len) != 0) { *n = 0; return NULL; }
        int k = io->ring_next; io->ring_next ^= 1;
        if (io->ring_cap[k] < len + 1) {
            uint8_t *b = realloc(io->ring[k], len + 1);
            if (!b) { fail(io, "out of memory reading %s (%u bytes)", name, len, 0); *n = 0; return NULL; }
            io->ring[k] = b; io->ring_cap[k] = len + 1;
        }
        if (fread(io->ring[k], 1, len, io->f) != len) { fail(io, "state ends inside chunk %s (%u bytes)", name, len, 0); *n = 0; return NULL; }
        *n = len;
        return io->ring[k];
    }
    uint32_t len;
    const uint8_t *src = read_chunk(io, name, &len);
    if (!src) { *n = 0; return NULL; }
    *n = len;
    return src;
}
