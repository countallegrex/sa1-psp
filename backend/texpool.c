/* texpool.c - texture memory in fixed chunks (texpool.h).
 *
 * Blocks sit in one list sorted by (chunk, offset); an allocation walks it
 * for the first gap that fits.  Compaction walks it once, sliding each block
 * to the lowest free position: the order never changes and no block moves
 * up, so a memmove per block is safe.
 *
 * Part of sa1-psp (MIT). */
#include "texpool.h"
#include <string.h>

#define MAX_BLOCKS 4096
#define MAX_CHUNKS 64

typedef struct {
    uint32_t off, size, last;
    int owner, next;
    uint16_t chunk;
    uint8_t cache, nodrop;
} block;

static block blk[MAX_BLOCKS];
static int head = -1, free_ids[MAX_BLOCKS], nfree_ids;
static void *chunk[MAX_CHUNKS];
static int nchunks, max_chunks, is_virt, inited;
static uint32_t now;
static texpool_ops ops;
static texpool_stats st;

void texpool_init(const texpool_ops *o, int is_virtual, uint32_t maxc) {
    ops = *o;
    is_virt = is_virtual;
    max_chunks = maxc && maxc < MAX_CHUNKS ? (int)maxc : MAX_CHUNKS;
    head = -1; nchunks = 0;
    nfree_ids = 0;
    for (int i = MAX_BLOCKS - 1; i >= 0; i--) free_ids[nfree_ids++] = i;
    memset(&st, 0, sizeof st);
    inited = 1;
}

const texpool_stats *texpool_get_stats(void) { st.chunks = (uint32_t)nchunks; return &st; }
void texpool_set_frame(uint32_t frame) { now = frame; }
void texpool_touch(int b) { if (b >= 0) blk[b].last = now; }

static uint8_t *addr_of(int b) { return (uint8_t *)chunk[blk[b].chunk] + blk[b].off; }

/* The first gap of `size` bytes: its chunk, offset and the block before it
 * in the list (-1 = the head). */
static int first_fit(uint32_t size, int *pc, uint32_t *poff, int *pprev) {
    int prev = -1, b = head;
    for (int c = 0; c < nchunks; c++) {
        uint32_t cur = 0;
        while (b >= 0 && blk[b].chunk == c) {
            if (blk[b].off - cur >= size) goto found;
            cur = blk[b].off + blk[b].size;
            prev = b; b = blk[b].next;
        }
        if (TEXPOOL_CHUNK - cur >= size) goto found;
        continue;
    found:
        *pc = c; *poff = cur; *pprev = prev;
        return 1;
    }
    return 0;
}

static void compact(void) {
    int c = 0;
    uint32_t o = 0;
    for (int b = head; b >= 0; b = blk[b].next) {
        if (o + blk[b].size > TEXPOOL_CHUNK) { c++; o = 0; }
        if (c != blk[b].chunk || o != blk[b].off) {
            uint8_t *from = addr_of(b);
            blk[b].chunk = (uint16_t)c; blk[b].off = o;
            if (!is_virt) memmove(addr_of(b), from, blk[b].size);
            st.moved_bytes += blk[b].size;
            ops.moved(blk[b].owner, addr_of(b));
        }
        o += blk[b].size;
    }
    /* Trailing chunks left empty go back to the heap. */
    int keep = head < 0 ? 0 : (o ? c + 1 : c);
    while (nchunks > keep) ops.chunk_free(chunk[--nchunks]);
    st.compactions++;
}

/* The least recently drawn block that may be evicted (cache) or shrunk,
 * drawn before frame `before`; -1 = none. */
static int pick(int cache, uint32_t before) {
    int best = -1;
    for (int b = head; b >= 0; b = blk[b].next) {
        if (blk[b].cache != cache || (!cache && blk[b].nodrop) || blk[b].last >= before) continue;
        if (best < 0 || blk[b].last < blk[best].last || (blk[b].last == blk[best].last && blk[b].size > blk[best].size)) best = b;
    }
    return best;
}

int texpool_alloc(uint32_t size, int owner, int cache, int may_reclaim, uint8_t **addr) {
    size = (size + 15u) & ~15u;
    if (!inited || size == 0 || size > TEXPOOL_CHUNK || !nfree_ids) return -1;
    /* Compaction moves the whole pool: after one, reclaim at least `margin`
     * more before the next (one per dropped level moved 300 MB on the
     * Emerald Coast route). */
    uint32_t margin = size > (256u << 10) ? size : (256u << 10), at_compact = 0;
    int compacted = 0;
    for (;;) {
        int c, prev;
        uint32_t off;
        if (first_fit(size, &c, &off, &prev)) {
            int b = free_ids[--nfree_ids];
            blk[b].off = off; blk[b].size = size; blk[b].owner = owner; blk[b].chunk = (uint16_t)c;
            blk[b].cache = (uint8_t)(cache != 0); blk[b].nodrop = 0; blk[b].last = now;
            if (prev < 0) { blk[b].next = head; head = b; }
            else { blk[b].next = blk[prev].next; blk[prev].next = b; }
            st.used += size;
            if (st.used > st.peak_used) st.peak_used = st.used;
            *addr = addr_of(b);
            return b;
        }
        if (nchunks < max_chunks) {
            void *ch = ops.chunk_alloc();
            if (ch) {
                chunk[nchunks++] = ch;
                if ((uint32_t)nchunks > st.peak_chunks) st.peak_chunks = (uint32_t)nchunks;
                continue;
            }
            st.refused_chunks++;
        }
        if (!may_reclaim) break;
        uint32_t freeb = (uint32_t)nchunks * TEXPOOL_CHUNK - st.used;
        if (freeb >= size && (!compacted || freeb >= at_compact + margin)) { compact(); compacted = 1; at_compact = freeb; continue; }
        int v = pick(1, 0xFFFFFFFFu);
        if (v >= 0) { ops.evict(blk[v].owner); st.evictions++; continue; }
        v = pick(0, now > TEXPOOL_IDLE ? now - TEXPOOL_IDLE : 0);
        if (v < 0) v = pick(0, now);
        if (v < 0) v = pick(0, 0xFFFFFFFFu);   /* even this frame's loads: half resolution beats white */
        if (v >= 0) {
            uint32_t ns = ops.drop_top(blk[v].owner);
            if (ns) {
                ns = (ns + 15u) & ~15u;
                st.used -= blk[v].size - ns;
                blk[v].size = ns;
                st.drops++;
            } else blk[v].nodrop = 1;
            continue;
        }
        /* nothing left to reclaim: a last compaction if space came free since */
        if (freeb >= size && freeb != at_compact) { compact(); compacted = 1; at_compact = freeb; continue; }
        break;
    }
    st.failures++;
    return -1;
}

int texpool_make_room(uint32_t size) {
    uint8_t *a;
    int b = texpool_alloc(size, -1, 0, 1, &a);
    if (b < 0) return 0;
    texpool_free(b);
    return 1;
}

void texpool_free(int b) {
    if (b < 0 || b >= MAX_BLOCKS) return;
    int *p = &head;
    while (*p >= 0 && *p != b) p = &blk[*p].next;
    if (*p != b) return;
    *p = blk[b].next;
    st.used -= blk[b].size;
    free_ids[nfree_ids++] = b;
}

uint32_t texpool_psp_bytes(int w, int h, int bits, int max_levels, int *levels) {
    int n = 1;
    while (n < max_levels && (w >> n) >= 8 && (h >> n) >= 8) n++;
    uint32_t total = 0;
    for (int l = 0; l < n; l++) {
        uint32_t rb = (uint32_t)(w >> l) * (uint32_t)bits / 8u;
        if (rb < 16) rb = 16;
        rb = (rb + 15u) & ~15u;
        total += rb * (((uint32_t)(h >> l) + 7u) & ~7u);
    }
    if (levels) *levels = n;
    return total;
}
