/* texpool.h - texture memory in fixed chunks, apart from the rest of the heap.
 *
 * The PSP keeps every texture in main RAM.  Allocated one by one from the
 * heap, textures and the game's other blocks interleave, and after a few
 * level changes the free space is there but in pieces too small for the
 * next level's textures, which then fail to build and draw white.  The pool
 * takes memory from the heap (and on the PSP, EDRAM) in chunks (1 MB) that only textures use, packs
 * textures into them and, when a texture does not fit:
 *   1. takes another chunk while the heap keeps a reserve free;
 *   2. compacts: moves every texture down so the free space is one piece
 *      (not while a display list holds texture addresses);
 *   3. evicts cache copies (the PSP's mirrored variants, rebuilt on use)
 *      not drawn for TEXPOOL_IDLE frames, least recently drawn first;
 *   4. drops the top mip level of the least recently drawn textures, those
 *      not drawn for TEXPOOL_IDLE frames first; then evicts the cache
 *      copies still in use; then drops levels of any texture not drawn this
 *      frame, then (a level larger than the pool) any: the texture stays,
 *      at half the resolution.
 * Steps 2-4 only when the caller says it may (no display list holds
 * texture addresses).  Chunks are never given back: the heap's other
 * users took a freed chunk, and the pool could not grow again.
 *
 * The desktop runs the same pool on virtual chunks (no memory, no moves)
 * with SA1_TEX_BUDGET, sized as the PSP sizes textures, so a texture that
 * would fail on the PSP fails (draws white) on the desktop too.
 *
 * Part of sa1-psp (MIT). */
#pragma once
#include <stdint.h>

#define TEXPOOL_CHUNK (1u << 20)
#define TEXPOOL_IDLE 60

typedef struct texpool_ops {
    /* A chunk of TEXPOOL_CHUNK bytes from the heap, NULL = refused.  Virtual
     * pools get any non-NULL token and never touch it. */
    void *(*chunk_alloc)(void);
    /* The owner's block now starts at addr (compaction). */
    void (*moved)(int owner, uint8_t *addr);
    /* Release a cache-copy owner (it calls texpool_free itself). */
    void (*evict)(int owner);
    /* Drop the owner's top mip level, moving the rest to the block's start;
     * returns the new size, 0 when it has a single level left. */
    uint32_t (*drop_top)(int owner);
} texpool_ops;

typedef struct texpool_stats {
    uint32_t chunks, used, peak_used, peak_chunks, compactions, moved_bytes, evictions, drops, failures, refused_chunks;
} texpool_stats;

void texpool_init(const texpool_ops *ops, int is_virtual, uint32_t max_chunks);
/* Allocates size bytes (16-aligned) for owner; returns the block id (>= 0)
 * and its address, or -1.  cache: an evictable copy, which may take room
 * from other copies (steps 2-3) but never a level of a game texture (4).
 * may_reclaim: steps 2-4 are allowed (no display list refers to texture
 * memory). */
int texpool_alloc(uint32_t size, int owner, int cache, int may_reclaim, uint8_t **addr);
void texpool_free(int blk);
/* Reclaims until size bytes fit (between frames), then frees them again:
 * room for the mirror variants that failed inside the last frame.  1 = done. */
int texpool_make_room(uint32_t size);
/* The block was drawn in this frame. */
void texpool_touch(int blk);
void texpool_set_frame(uint32_t frame);
const texpool_stats *texpool_get_stats(void);

/* Bytes the PSP backend gives a w x h texture of `bits` per pixel with up to
 * max_levels mip levels (draw_gu.c texture_build: rows padded to 16 bytes,
 * height to 8 rows, levels down to 8 x 8). */
uint32_t texpool_psp_bytes(int w, int h, int bits, int max_levels, int *levels);
